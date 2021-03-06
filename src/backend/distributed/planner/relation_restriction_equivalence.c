/*
 * relation_restriction_equivalence.c
 *
 * This file contains functions helper functions for planning
 * queries with colocated tables and subqueries.
 *
 * Copyright (c) 2017-2017, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "distributed/multi_planner.h"
#include "distributed/multi_logical_planner.h"
#include "distributed/multi_logical_optimizer.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/relation_restriction_equivalence.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "parser/parsetree.h"
#include "optimizer/pathnode.h"

static uint32 attributeEquivalenceId = 1;


/*
 * AttributeEquivalenceClass
 *
 * Whenever we find an equality clause A = B, where both A and B originates from
 * relation attributes (i.e., not random expressions), we create an
 * AttributeEquivalenceClass to record this knowledge. If we later find another
 * equivalence B = C, we create another AttributeEquivalenceClass. Finally, we can
 * apply transitivity rules and generate a new AttributeEquivalenceClass which includes
 * A, B and C.
 *
 * Note that equality among the members are identified by the varattno and rteIdentity.
 */
typedef struct AttributeEquivalenceClass
{
	uint32 equivalenceId;
	List *equivalentAttributes;
} AttributeEquivalenceClass;

/*
 *  AttributeEquivalenceClassMember - one member expression of an
 *  AttributeEquivalenceClass. The important thing to consider is that
 *  the class member contains "rteIndentity" field. Note that each RTE_RELATION
 *  is assigned a unique rteIdentity in AssignRTEIdentities() function.
 *
 *  "varno" and "varattno" is directly used from a Var clause that is being added
 *  to the attribute equivalence. Since we only use this class for relations, the member
 *  also includes the relation id field.
 */
typedef struct AttributeEquivalenceClassMember
{
	Oid relationId;
	int rteIdentity;
	Index varno;
	AttrNumber varattno;
} AttributeEquivalenceClassMember;


static Var * FindTranslatedVar(List *appendRelList, Oid relationOid,
							   Index relationRteIndex, Index *partitionKeyIndex);
static bool EquivalenceListContainsRelationsEquality(List *attributeEquivalenceList,
													 RelationRestrictionContext *
													 restrictionContext);
static List * GenerateAttributeEquivalencesForRelationRestrictions(
	RelationRestrictionContext *restrictionContext);
static AttributeEquivalenceClass * AttributeEquivalenceClassForEquivalenceClass(
	EquivalenceClass *plannerEqClass, RelationRestriction *relationRestriction);
static void AddToAttributeEquivalenceClass(AttributeEquivalenceClass **
										   attributeEquivalanceClass,
										   PlannerInfo *root, Var *varToBeAdded);
static void AddRteSubqueryToAttributeEquivalenceClass(AttributeEquivalenceClass *
													  *attributeEquivalanceClass,
													  RangeTblEntry *
													  rangeTableEntry,
													  PlannerInfo *root,
													  Var *varToBeAdded);
static Query * GetTargetSubquery(PlannerInfo *root, RangeTblEntry *rangeTableEntry,
								 Var *varToBeAdded);
static void AddUnionAllSetOperationsToAttributeEquivalenceClass(
	AttributeEquivalenceClass **
	attributeEquivalanceClass,
	PlannerInfo *root,
	Var *varToBeAdded);
static void AddUnionSetOperationsToAttributeEquivalenceClass(AttributeEquivalenceClass **
															 attributeEquivalenceClass,
															 PlannerInfo *root,
															 SetOperationStmt *
															 setOperation,
															 Var *varToBeAdded);
static void AddRteRelationToAttributeEquivalenceClass(AttributeEquivalenceClass **
													  attrEquivalenceClass,
													  RangeTblEntry *rangeTableEntry,
													  Var *varToBeAdded);
static Var * GetVarFromAssignedParam(List *parentPlannerParamList,
									 Param *plannerParam);
static List * GenerateAttributeEquivalencesForJoinRestrictions(JoinRestrictionContext
															   *joinRestrictionContext);
static bool AttributeClassContainsAttributeClassMember(AttributeEquivalenceClassMember *
													   inputMember,
													   AttributeEquivalenceClass *
													   attributeEquivalenceClass);
static List * AddAttributeClassToAttributeClassList(List *attributeEquivalenceList,
													AttributeEquivalenceClass *
													attributeEquivalance);
static bool AttributeEquivalancesAreEqual(AttributeEquivalenceClass *
										  firstAttributeEquivalance,
										  AttributeEquivalenceClass *
										  secondAttributeEquivalance);
static AttributeEquivalenceClass * GenerateCommonEquivalence(List *
															 attributeEquivalenceList);
static void ListConcatUniqueAttributeClassMemberLists(AttributeEquivalenceClass **
													  firstClass,
													  AttributeEquivalenceClass *
													  secondClass);
static Index RelationRestrictionPartitionKeyIndex(RelationRestriction *
												  relationRestriction);


/*
 * SafeToPushdownUnionSubquery returns true if all the relations are returns
 * partition keys in the same ordinal position and there is no reference table
 * exists.
 *
 * Note that the function expects (and asserts) the input query to be a top
 * level union query defined by TopLevelUnionQuery().
 *
 * Lastly, the function fails to produce correct output if the target lists contains
 * multiple partition keys on the target list such as the following:
 *
 *   select count(*) from (
 *       select user_id, user_id from users_table
 *   union
 *       select 2, user_id  from users_table) u;
 *
 * For the above query, although the second item in the target list make this query
 * safe to push down, the function would fail to return true.
 */
bool
SafeToPushdownUnionSubquery(RelationRestrictionContext *restrictionContext)
{
	Index unionQueryPartitionKeyIndex = 0;
	AttributeEquivalenceClass *attributeEquivalance =
		palloc0(sizeof(AttributeEquivalenceClass));
	ListCell *relationRestrictionCell = NULL;

	attributeEquivalance->equivalenceId = attributeEquivalenceId++;

	foreach(relationRestrictionCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationRestriction = lfirst(relationRestrictionCell);
		Oid relationId = relationRestriction->relationId;
		Index partitionKeyIndex = InvalidAttrNumber;
		PlannerInfo *relationPlannerRoot = relationRestriction->plannerInfo;
		List *targetList = relationPlannerRoot->parse->targetList;
		List *appendRelList = relationPlannerRoot->append_rel_list;
		Var *varToBeAdded = NULL;
		TargetEntry *targetEntryToAdd = NULL;

		/*
		 * Although it is not the best place to error out when facing with reference
		 * tables, we decide to error out here. Otherwise, we need to add equality
		 * for each reference table and it is more complex to implement. In the
		 * future implementation all checks will be gathered to single point.
		 */
		if (PartitionMethod(relationId) == DISTRIBUTE_BY_NONE)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot pushdown this query"),
							errdetail(
								"Reference tables are not allowed with set operations")));
		}

		/*
		 * We first check whether UNION ALLs are pulled up or not. Note that Postgres
		 * planner creates AppendRelInfos per each UNION ALL query that is pulled up.
		 * Then, postgres stores the related information in the append_rel_list on the
		 * plannerInfo struct.
		 */
		if (appendRelList != NULL)
		{
			varToBeAdded = FindTranslatedVar(appendRelList,
											 relationRestriction->relationId,
											 relationRestriction->index,
											 &partitionKeyIndex);

			/* union does not have partition key in the target list */
			if (partitionKeyIndex == 0)
			{
				return false;
			}
		}
		else
		{
			partitionKeyIndex =
				RelationRestrictionPartitionKeyIndex(relationRestriction);

			/* union does not have partition key in the target list */
			if (partitionKeyIndex == 0)
			{
				return false;
			}

			targetEntryToAdd = list_nth(targetList, partitionKeyIndex - 1);
			if (!IsA(targetEntryToAdd->expr, Var))
			{
				return false;
			}

			varToBeAdded = (Var *) targetEntryToAdd->expr;
		}

		/*
		 * If the first relation doesn't have partition key on the target
		 * list of the query that the relation in, simply not allow to push down
		 * the query.
		 */
		if (partitionKeyIndex == InvalidAttrNumber)
		{
			return false;
		}

		/*
		 * We find the first relations partition key index in the target list. Later,
		 * we check whether all the relations have partition keys in the
		 * same position.
		 */
		if (unionQueryPartitionKeyIndex == InvalidAttrNumber)
		{
			unionQueryPartitionKeyIndex = partitionKeyIndex;
		}
		else if (unionQueryPartitionKeyIndex != partitionKeyIndex)
		{
			return false;
		}

		AddToAttributeEquivalenceClass(&attributeEquivalance, relationPlannerRoot,
									   varToBeAdded);
	}

	return EquivalenceListContainsRelationsEquality(list_make1(attributeEquivalance),
													restrictionContext);
}


/*
 * FindTranslatedVar iterates on the appendRelList and tries to find a translated
 * child var identified by the relation id and the relation rte index.
 *
 * Note that postgres translates UNION ALL target list elements into translated_vars
 * list on the corresponding AppendRelInfo struct. For details, see the related
 * structs.
 *
 * The function returns NULL if it cannot find a translated var.
 */
static Var *
FindTranslatedVar(List *appendRelList, Oid relationOid, Index relationRteIndex,
				  Index *partitionKeyIndex)
{
	ListCell *appendRelCell = NULL;
	AppendRelInfo *targetAppendRelInfo = NULL;
	ListCell *translatedVarCell = NULL;
	AttrNumber childAttrNumber = 0;
	Var *relationPartitionKey = NULL;
	List *translaterVars = NULL;

	*partitionKeyIndex = 0;

	/* iterate on the queries that are part of UNION ALL subselects */
	foreach(appendRelCell, appendRelList)
	{
		AppendRelInfo *appendRelInfo = (AppendRelInfo *) lfirst(appendRelCell);

		/*
		 * We're only interested in the child rel that is equal to the
		 * relation we're investigating.
		 */
		if (appendRelInfo->child_relid == relationRteIndex)
		{
			targetAppendRelInfo = appendRelInfo;
			break;
		}
	}

	/* we couldn't find the necessary append rel info */
	if (targetAppendRelInfo == NULL)
	{
		return NULL;
	}

	relationPartitionKey = DistPartitionKey(relationOid);

	translaterVars = targetAppendRelInfo->translated_vars;
	foreach(translatedVarCell, translaterVars)
	{
		Node *targetNode = (Node *) lfirst(translatedVarCell);
		Var *targetVar = NULL;

		childAttrNumber++;

		if (!IsA(targetNode, Var))
		{
			continue;
		}

		targetVar = (Var *) lfirst(translatedVarCell);
		if (targetVar->varno == relationRteIndex &&
			targetVar->varattno == relationPartitionKey->varattno)
		{
			*partitionKeyIndex = childAttrNumber;

			return targetVar;
		}
	}

	return NULL;
}


/*
 * RestrictionEquivalenceForPartitionKeys aims to deduce whether each of the RTE_RELATION
 * is joined with at least one another RTE_RELATION on their partition keys. If each
 * RTE_RELATION follows the above rule, we can conclude that all RTE_RELATIONs are
 * joined on their partition keys.
 *
 * The function returns true if all relations are joined on their partition keys.
 * Otherwise, the function returns false. In order to support reference tables
 * with subqueries, equality between attributes of reference tables and partition
 * key of distributed tables are also considered.
 *
 * In order to do that, we invented a new equivalence class namely:
 * AttributeEquivalenceClass. In very simple words, a AttributeEquivalenceClass is
 * identified by an unique id and consists of a list of AttributeEquivalenceMembers.
 *
 * Each AttributeEquivalenceMember is designed to identify attributes uniquely within the
 * whole query. The necessity of this arise since varno attributes are defined within
 * a single level of a query. Instead, here we want to identify each RTE_RELATION uniquely
 * and try to find equality among each RTE_RELATION's partition key.
 *
 * Each equality among RTE_RELATION is saved using an AttributeEquivalenceClass where
 * each member attribute is identified by a AttributeEquivalenceMember. In the final
 * step, we try generate a common attribute equivalence class that holds as much as
 * AttributeEquivalenceMembers whose attributes are a partition keys.
 *
 * RestrictionEquivalenceForPartitionKeys uses both relation restrictions and join restrictions
 * to find as much as information that Postgres planner provides to extensions. For the
 * details of the usage, please see GenerateAttributeEquivalencesForRelationRestrictions()
 * and GenerateAttributeEquivalencesForJoinRestrictions()
 */
bool
RestrictionEquivalenceForPartitionKeys(PlannerRestrictionContext *
									   plannerRestrictionContext)
{
	RelationRestrictionContext *restrictionContext =
		plannerRestrictionContext->relationRestrictionContext;
	JoinRestrictionContext *joinRestrictionContext =
		plannerRestrictionContext->joinRestrictionContext;

	List *relationRestrictionAttributeEquivalenceList = NIL;
	List *joinRestrictionAttributeEquivalenceList = NIL;
	List *allAttributeEquivalenceList = NIL;

	uint32 totalRelationCount = list_length(restrictionContext->relationRestrictionList);

	/*
	 * If the query includes only one relation, we should not check the partition
	 * column equality. Single table should not need to fetch data from other nodes
	 * except it's own node(s).
	 */
	if (totalRelationCount == 1)
	{
		return true;
	}

	/* reset the equivalence id counter per call to prevent overflows */
	attributeEquivalenceId = 1;

	relationRestrictionAttributeEquivalenceList =
		GenerateAttributeEquivalencesForRelationRestrictions(restrictionContext);
	joinRestrictionAttributeEquivalenceList =
		GenerateAttributeEquivalencesForJoinRestrictions(joinRestrictionContext);

	allAttributeEquivalenceList =
		list_concat(relationRestrictionAttributeEquivalenceList,
					joinRestrictionAttributeEquivalenceList);

	return EquivalenceListContainsRelationsEquality(allAttributeEquivalenceList,
													restrictionContext);
}


/*
 * EquivalenceListContainsRelationsEquality gets a list of attributed equivalence
 * list and a relation restriction context. The function first generates a common
 * equivalence class out of the attributeEquivalenceList. Later, the function checks
 * whether all the relations exists in the common equivalence class.
 *
 */
static bool
EquivalenceListContainsRelationsEquality(List *attributeEquivalenceList,
										 RelationRestrictionContext *restrictionContext)
{
	AttributeEquivalenceClass *commonEquivalenceClass = NULL;
	ListCell *commonEqClassCell = NULL;
	ListCell *relationRestrictionCell = NULL;
	Relids commonRteIdentities = NULL;

	/*
	 * In general we're trying to expand existing the equivalence classes to find a
	 * common equivalence class. The main goal is to test whether this main class
	 * contains all partition keys of the existing relations.
	 */
	commonEquivalenceClass = GenerateCommonEquivalence(attributeEquivalenceList);

	/* add the rte indexes of relations to a bitmap */
	foreach(commonEqClassCell, commonEquivalenceClass->equivalentAttributes)
	{
		AttributeEquivalenceClassMember *classMember =
			(AttributeEquivalenceClassMember *) lfirst(commonEqClassCell);
		int rteIdentity = classMember->rteIdentity;

		commonRteIdentities = bms_add_member(commonRteIdentities, rteIdentity);
	}

	/* check whether all relations exists in the main restriction list */
	foreach(relationRestrictionCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationRestriction =
			(RelationRestriction *) lfirst(relationRestrictionCell);
		int rteIdentity = GetRTEIdentity(relationRestriction->rte);

		if (!bms_is_member(rteIdentity, commonRteIdentities))
		{
			return false;
		}
	}

	return true;
}


/*
 * GenerateAttributeEquivalencesForRelationRestrictions gets a relation restriction
 * context and returns a list of AttributeEquivalenceClass.
 *
 * The algorithm followed can be summarized as below:
 *
 * - Per relation restriction
 *     - Per plannerInfo's eq_class
 *         - Create an AttributeEquivalenceClass
 *         - Add all Vars that appear in the plannerInfo's
 *           eq_class to the AttributeEquivalenceClass
 *               - While doing that, consider LATERAL vars as well.
 *                 See GetVarFromAssignedParam() for the details. Note
 *                 that we're using parentPlannerInfo while adding the
 *                 LATERAL vars given that we rely on that plannerInfo.
 *
 */
static List *
GenerateAttributeEquivalencesForRelationRestrictions(RelationRestrictionContext
													 *restrictionContext)
{
	List *attributeEquivalenceList = NIL;
	ListCell *relationRestrictionCell = NULL;

	foreach(relationRestrictionCell, restrictionContext->relationRestrictionList)
	{
		RelationRestriction *relationRestriction =
			(RelationRestriction *) lfirst(relationRestrictionCell);
		List *equivalenceClasses = relationRestriction->plannerInfo->eq_classes;
		ListCell *equivalenceClassCell = NULL;

		foreach(equivalenceClassCell, equivalenceClasses)
		{
			EquivalenceClass *plannerEqClass =
				(EquivalenceClass *) lfirst(equivalenceClassCell);

			AttributeEquivalenceClass *attributeEquivalance =
				AttributeEquivalenceClassForEquivalenceClass(plannerEqClass,
															 relationRestriction);

			attributeEquivalenceList =
				AddAttributeClassToAttributeClassList(attributeEquivalenceList,
													  attributeEquivalance);
		}
	}

	return attributeEquivalenceList;
}


/*
 * AttributeEquivalenceClassForEquivalenceClass is a helper function for
 * GenerateAttributeEquivalencesForRelationRestrictions. The function takes an
 * EquivalenceClass and the relation restriction that the equivalence class
 * belongs to. The function returns an AttributeEquivalenceClass that is composed
 * of ec_members that are simple Var references.
 *
 * The function also takes case of LATERAL joins by simply replacing the PARAM_EXEC
 * with the corresponding expression.
 */
static AttributeEquivalenceClass *
AttributeEquivalenceClassForEquivalenceClass(EquivalenceClass *plannerEqClass,
											 RelationRestriction *relationRestriction)
{
	AttributeEquivalenceClass *attributeEquivalance =
		palloc0(sizeof(AttributeEquivalenceClass));
	ListCell *equivilanceMemberCell = NULL;
	PlannerInfo *plannerInfo = relationRestriction->plannerInfo;

	attributeEquivalance->equivalenceId = attributeEquivalenceId++;

	foreach(equivilanceMemberCell, plannerEqClass->ec_members)
	{
		EquivalenceMember *equivalenceMember =
			(EquivalenceMember *) lfirst(equivilanceMemberCell);
		Node *equivalenceNode = strip_implicit_coercions(
			(Node *) equivalenceMember->em_expr);
		Expr *strippedEquivalenceExpr = (Expr *) equivalenceNode;

		Var *expressionVar = NULL;

		if (IsA(strippedEquivalenceExpr, Param))
		{
			List *parentParamList = relationRestriction->parentPlannerParamList;
			Param *equivalenceParam = (Param *) strippedEquivalenceExpr;

			expressionVar = GetVarFromAssignedParam(parentParamList,
													equivalenceParam);
			if (expressionVar)
			{
				AddToAttributeEquivalenceClass(&attributeEquivalance,
											   relationRestriction->parentPlannerInfo,
											   expressionVar);
			}
		}
		else if (IsA(strippedEquivalenceExpr, Var))
		{
			expressionVar = (Var *) strippedEquivalenceExpr;
			AddToAttributeEquivalenceClass(&attributeEquivalance, plannerInfo,
										   expressionVar);
		}
	}

	return attributeEquivalance;
}


/*
 * GetVarFromAssignedParam returns the Var that is assigned to the given
 * plannerParam if its kind is PARAM_EXEC.
 *
 * If the paramkind is not equal to PARAM_EXEC the function returns NULL. Similarly,
 * if there is no var that the given param is assigned to, the function returns NULL.
 *
 * Rationale behind this function:
 *
 *   While iterating through the equivalence classes of RTE_RELATIONs, we
 *   observe that there are PARAM type of equivalence member expressions for
 *   the RTE_RELATIONs which actually belong to lateral vars from the other query
 *   levels.
 *
 *   We're also keeping track of the RTE_RELATION's parent_root's
 *   plan_param list which is expected to hold the parameters that are required
 *   for its lower level queries as it is documented:
 *
 *        plan_params contains the expressions that this query level needs to
 *        make available to a lower query level that is currently being planned.
 *
 *   This function is a helper function to iterate through the parent query's
 *   plan_params and looks for the param that the equivalence member has. The
 *   comparison is done via the "paramid" field. Finally, if the found parameter's
 *   item is a Var, we conclude that Postgres standard_planner replaced the Var
 *   with the Param on assign_param_for_var() function
 *   @src/backend/optimizer//plan/subselect.c.
 *
 */
static Var *
GetVarFromAssignedParam(List *parentPlannerParamList, Param *plannerParam)
{
	Var *assignedVar = NULL;
	ListCell *plannerParameterCell = NULL;

	Assert(plannerParam != NULL);

	/* we're only interested in parameters that Postgres added for execution */
	if (plannerParam->paramkind != PARAM_EXEC)
	{
		return NULL;
	}

	foreach(plannerParameterCell, parentPlannerParamList)
	{
		PlannerParamItem *plannerParamItem =
			(PlannerParamItem *) lfirst(plannerParameterCell);

		if (plannerParamItem->paramId != plannerParam->paramid)
		{
			continue;
		}

		/* TODO: Should we consider PlaceHolderVar?? */
		if (!IsA(plannerParamItem->item, Var))
		{
			continue;
		}

		assignedVar = (Var *) plannerParamItem->item;

		break;
	}

	return assignedVar;
}


/*
 * GenerateCommonEquivalence gets a list of unrelated AttributeEquiavalanceClass
 * whose all members are partition keys or a column of reference table.
 *
 * With the equivalence classes, the function follows the algorithm
 * outlined below:
 *
 *     - Add the first equivalence class to the common equivalence class
 *     - Then, iterate on the remaining equivalence classes
 *          - If any of the members equal to the common equivalence class
 *            add all the members of the equivalence class to the common
 *            class
 *          - Start the iteration from the beginning. The reason is that
 *            in case any of the classes we've passed is equivalent to the
 *            newly added one. To optimize the algorithm, we utilze the
 *            equivalence class ids and skip the ones that are already added.
 *      - Finally, return the common equivalence class.
 */
static AttributeEquivalenceClass *
GenerateCommonEquivalence(List *attributeEquivalenceList)
{
	AttributeEquivalenceClass *commonEquivalenceClass = NULL;
	AttributeEquivalenceClass *firstEquivalenceClass = NULL;
	Bitmapset *addedEquivalenceIds = NULL;
	uint32 equivalenceListSize = list_length(attributeEquivalenceList);
	uint32 equivalenceClassIndex = 0;

	commonEquivalenceClass = palloc0(sizeof(AttributeEquivalenceClass));
	commonEquivalenceClass->equivalenceId = 0;

	/* think more on this. */
	if (equivalenceListSize < 1)
	{
		return commonEquivalenceClass;
	}

	/* setup the initial state of the main equivalence class */
	firstEquivalenceClass = linitial(attributeEquivalenceList);
	commonEquivalenceClass->equivalentAttributes =
		firstEquivalenceClass->equivalentAttributes;
	addedEquivalenceIds = bms_add_member(addedEquivalenceIds,
										 firstEquivalenceClass->equivalenceId);

	for (; equivalenceClassIndex < equivalenceListSize; ++equivalenceClassIndex)
	{
		AttributeEquivalenceClass *currentEquivalenceClass =
			list_nth(attributeEquivalenceList, equivalenceClassIndex);
		ListCell *equivalenceMemberCell = NULL;

		/*
		 * This is an optimization. If we already added the same equivalence class,
		 * we could skip it since we've already added all the relevant equivalence
		 * members.
		 */
		if (bms_is_member(currentEquivalenceClass->equivalenceId, addedEquivalenceIds))
		{
			continue;
		}

		foreach(equivalenceMemberCell, currentEquivalenceClass->equivalentAttributes)
		{
			AttributeEquivalenceClassMember *attributeEquialanceMember =
				(AttributeEquivalenceClassMember *) lfirst(equivalenceMemberCell);

			if (AttributeClassContainsAttributeClassMember(attributeEquialanceMember,
														   commonEquivalenceClass))
			{
				ListConcatUniqueAttributeClassMemberLists(&commonEquivalenceClass,
														  currentEquivalenceClass);

				addedEquivalenceIds = bms_add_member(addedEquivalenceIds,
													 currentEquivalenceClass->
													 equivalenceId);

				/*
				 * It seems inefficient to start from the beginning.
				 * But, we should somehow restart from the beginning to test that
				 * whether the already skipped ones are equal or not.
				 */
				equivalenceClassIndex = 0;

				break;
			}
		}
	}

	return commonEquivalenceClass;
}


/*
 * ListConcatUniqueAttributeClassMemberLists gets two attribute equivalence classes. It
 * basically concatenates attribute equivalence member lists uniquely and updates the
 * firstClass' member list with the list.
 *
 * Basically, the function iterates over the secondClass' member list and checks whether
 * it already exists in the firstClass' member list. If not, the member is added to the
 * firstClass.
 */
static void
ListConcatUniqueAttributeClassMemberLists(AttributeEquivalenceClass **firstClass,
										  AttributeEquivalenceClass *secondClass)
{
	ListCell *equivalenceClassMemberCell = NULL;
	List *equivalenceMemberList = secondClass->equivalentAttributes;

	foreach(equivalenceClassMemberCell, equivalenceMemberList)
	{
		AttributeEquivalenceClassMember *newEqMember =
			(AttributeEquivalenceClassMember *) lfirst(equivalenceClassMemberCell);

		if (AttributeClassContainsAttributeClassMember(newEqMember, *firstClass))
		{
			continue;
		}

		(*firstClass)->equivalentAttributes = lappend((*firstClass)->equivalentAttributes,
													  newEqMember);
	}
}


/*
 * GenerateAttributeEquivalencesForJoinRestrictions gets a join restriction
 * context and returns a list of AttrributeEquivalenceClass.
 *
 * The algorithm followed can be summarized as below:
 *
 * - Per join restriction
 *     - Per RestrictInfo of the join restriction
 *     - Check whether the join restriction is in the form of (Var1 = Var2)
 *         - Create an AttributeEquivalenceClass
 *         - Add both Var1 and Var2 to the AttributeEquivalenceClass
 */
static List *
GenerateAttributeEquivalencesForJoinRestrictions(JoinRestrictionContext *
												 joinRestrictionContext)
{
	List *attributeEquivalenceList = NIL;
	ListCell *joinRestrictionCell = NULL;

	foreach(joinRestrictionCell, joinRestrictionContext->joinRestrictionList)
	{
		JoinRestriction *joinRestriction =
			(JoinRestriction *) lfirst(joinRestrictionCell);
		ListCell *restrictionInfoList = NULL;

		foreach(restrictionInfoList, joinRestriction->joinRestrictInfoList)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(restrictionInfoList);
			OpExpr *restrictionOpExpr = NULL;
			Node *leftNode = NULL;
			Node *rightNode = NULL;
			Expr *strippedLeftExpr = NULL;
			Expr *strippedRightExpr = NULL;
			Var *leftVar = NULL;
			Var *rightVar = NULL;
			Expr *restrictionClause = rinfo->clause;
			AttributeEquivalenceClass *attributeEquivalance = NULL;

			if (!IsA(restrictionClause, OpExpr))
			{
				continue;
			}

			restrictionOpExpr = (OpExpr *) restrictionClause;
			if (list_length(restrictionOpExpr->args) != 2)
			{
				continue;
			}
			if (!OperatorImplementsEquality(restrictionOpExpr->opno))
			{
				continue;
			}

			leftNode = linitial(restrictionOpExpr->args);
			rightNode = lsecond(restrictionOpExpr->args);

			/* we also don't want implicit coercions */
			strippedLeftExpr = (Expr *) strip_implicit_coercions((Node *) leftNode);
			strippedRightExpr = (Expr *) strip_implicit_coercions((Node *) rightNode);

			if (!(IsA(strippedLeftExpr, Var) && IsA(strippedRightExpr, Var)))
			{
				continue;
			}

			leftVar = (Var *) strippedLeftExpr;
			rightVar = (Var *) strippedRightExpr;

			attributeEquivalance = palloc0(sizeof(AttributeEquivalenceClass));
			attributeEquivalance->equivalenceId = attributeEquivalenceId++;

			AddToAttributeEquivalenceClass(&attributeEquivalance,
										   joinRestriction->plannerInfo, leftVar);

			AddToAttributeEquivalenceClass(&attributeEquivalance,
										   joinRestriction->plannerInfo, rightVar);

			attributeEquivalenceList =
				AddAttributeClassToAttributeClassList(attributeEquivalenceList,
													  attributeEquivalance);
		}
	}

	return attributeEquivalenceList;
}


/*
 * AddToAttributeEquivalenceClass is a key function for building the attribute
 * equivalences. The function gets a plannerInfo, var and attribute equivalence
 * class. It searches for the RTE_RELATION(s) that the input var belongs to and
 * adds the found Var(s) to the input attribute equivalence class.
 *
 * Note that the input var could come from a subquery (i.e., not directly from an
 * RTE_RELATION). That's the reason we recursively call the function until the
 * RTE_RELATION found.
 *
 * The algorithm could be summarized as follows:
 *
 *    - If the RTE that corresponds to a relation
 *        - Generate an AttributeEquivalenceMember and add to the input
 *          AttributeEquivalenceClass
 *    - If the RTE that corresponds to a subquery
 *        - If the RTE that corresponds to a UNION ALL subquery
 *            - Iterate on each of the appendRels (i.e., each of the UNION ALL query)
 *            - Recursively add all children of the set operation's
 *              corresponding target entries
 *        - If the corresponding subquery entry is a UNION set operation
 *             - Recursively add all children of the set operation's
 *               corresponding target entries
 *        - If the corresponding subquery is a regular subquery (i.e., No set operations)
 *             - Recursively try to add the corresponding target entry to the
 *               equivalence class
 */
static void
AddToAttributeEquivalenceClass(AttributeEquivalenceClass **attributeEquivalanceClass,
							   PlannerInfo *root, Var *varToBeAdded)
{
	RangeTblEntry *rangeTableEntry = NULL;

	/* punt if it's a whole-row var rather than a plain column reference */
	if (varToBeAdded->varattno == InvalidAttrNumber)
	{
		return;
	}

	/* we also don't want to process ctid, tableoid etc */
	if (varToBeAdded->varattno < InvalidAttrNumber)
	{
		return;
	}

	rangeTableEntry = root->simple_rte_array[varToBeAdded->varno];
	if (rangeTableEntry->rtekind == RTE_RELATION)
	{
		AddRteRelationToAttributeEquivalenceClass(attributeEquivalanceClass,
												  rangeTableEntry,
												  varToBeAdded);
	}
	else if (rangeTableEntry->rtekind == RTE_SUBQUERY)
	{
		AddRteSubqueryToAttributeEquivalenceClass(attributeEquivalanceClass,
												  rangeTableEntry, root,
												  varToBeAdded);
	}
}


/*
 * AddRteSubqueryToAttributeEquivalenceClass adds the given var to the given
 * attribute equivalence class.
 *
 * The main algorithm is outlined in AddToAttributeEquivalenceClass().
 */
static void
AddRteSubqueryToAttributeEquivalenceClass(AttributeEquivalenceClass
										  **attributeEquivalanceClass,
										  RangeTblEntry *rangeTableEntry,
										  PlannerInfo *root, Var *varToBeAdded)
{
	RelOptInfo *baseRelOptInfo = find_base_rel(root, varToBeAdded->varno);
	TargetEntry *subqueryTargetEntry = NULL;
	Query *targetSubquery = GetTargetSubquery(root, rangeTableEntry, varToBeAdded);

	subqueryTargetEntry = get_tle_by_resno(targetSubquery->targetList,
										   varToBeAdded->varattno);

	/* if we fail to find corresponding target entry, do not proceed */
	if (subqueryTargetEntry == NULL || subqueryTargetEntry->resjunk)
	{
		return;
	}

	/* we're only interested in Vars */
	if (!IsA(subqueryTargetEntry->expr, Var))
	{
		return;
	}

	varToBeAdded = (Var *) subqueryTargetEntry->expr;

	/*
	 *  "inh" flag is set either when inheritance or "UNION ALL" exists in the
	 *  subquery. Here we're only interested in the "UNION ALL" case.
	 *
	 *  Else, we check one more thing: Does the subquery contain a "UNION" query.
	 *  If so, we recursively traverse all "UNION" tree and add the corresponding
	 *  target list elements to the attribute equivalence.
	 *
	 *  Finally, if it is a regular subquery (i.e., does not contain UNION or UNION ALL),
	 *  we simply recurse to find the corresponding RTE_RELATION to add to the
	 *  equivalence class.
	 *
	 *  Note that we're treating "UNION" and "UNION ALL" clauses differently given
	 *  that postgres planner process/plans them separately.
	 */
	if (rangeTableEntry->inh)
	{
		AddUnionAllSetOperationsToAttributeEquivalenceClass(attributeEquivalanceClass,
															root, varToBeAdded);
	}
	else if (targetSubquery->setOperations)
	{
		AddUnionSetOperationsToAttributeEquivalenceClass(attributeEquivalanceClass,
														 baseRelOptInfo->subroot,
														 (SetOperationStmt *)
														 targetSubquery->setOperations,
														 varToBeAdded);
	}
	else if (varToBeAdded && IsA(varToBeAdded, Var) && varToBeAdded->varlevelsup == 0)
	{
		AddToAttributeEquivalenceClass(attributeEquivalanceClass,
									   baseRelOptInfo->subroot, varToBeAdded);
	}
}


/*
 * GetTargetSubquery returns the corresponding subquery for the given planner root,
 * range table entry and the var.
 *
 * The aim of this function is to simplify extracting the subquery in case of "UNION ALL"
 * queries.
 */
static Query *
GetTargetSubquery(PlannerInfo *root, RangeTblEntry *rangeTableEntry, Var *varToBeAdded)
{
	Query *targetSubquery = NULL;

	/*
	 * For subqueries other than "UNION ALL", find the corresponding targetSubquery. See
	 * the details of how we process subqueries in the below comments.
	 */
	if (!rangeTableEntry->inh)
	{
		RelOptInfo *baseRelOptInfo = find_base_rel(root, varToBeAdded->varno);

		/* If the targetSubquery hasn't been planned yet, we have to punt */
		if (baseRelOptInfo->subroot == NULL)
		{
			return NULL;
		}

		Assert(IsA(baseRelOptInfo->subroot, PlannerInfo));

		targetSubquery = baseRelOptInfo->subroot->parse;
		Assert(IsA(targetSubquery, Query));
	}
	else
	{
		targetSubquery = rangeTableEntry->subquery;
	}

	return targetSubquery;
}


/*
 * AddUnionAllSetOperationsToAttributeEquivalenceClass recursively iterates on all the
 * append rels, sets the varno's accordingly and adds the
 * var the given equivalence class.
 */
static void
AddUnionAllSetOperationsToAttributeEquivalenceClass(AttributeEquivalenceClass **
													attributeEquivalanceClass,
													PlannerInfo *root,
													Var *varToBeAdded)
{
	List *appendRelList = root->append_rel_list;
	ListCell *appendRelCell = NULL;

	/* iterate on the queries that are part of UNION ALL subqueries */
	foreach(appendRelCell, appendRelList)
	{
		AppendRelInfo *appendRelInfo = (AppendRelInfo *) lfirst(appendRelCell);

		/*
		 * We're only interested in UNION ALL clauses and parent_reloid is invalid
		 * only for UNION ALL (i.e., equals to a legitimate Oid for inheritance)
		 */
		if (appendRelInfo->parent_reloid != InvalidOid)
		{
			continue;
		}

		/* set the varno accordingly for this specific child */
		varToBeAdded->varno = appendRelInfo->child_relid;

		AddToAttributeEquivalenceClass(attributeEquivalanceClass, root,
									   varToBeAdded);
	}
}


/*
 * AddUnionSetOperationsToAttributeEquivalenceClass recursively iterates on all the
 * setOperations and adds each corresponding target entry to the given equivalence
 * class.
 *
 * Although the function silently accepts INTERSECT and EXPECT set operations, they are
 * rejected later in the planning. We prefer this behavior to provide better error
 * messages.
 */
static void
AddUnionSetOperationsToAttributeEquivalenceClass(AttributeEquivalenceClass **
												 attributeEquivalenceClass,
												 PlannerInfo *root,
												 SetOperationStmt *setOperation,
												 Var *varToBeAdded)
{
	List *rangeTableIndexList = NIL;
	ListCell *rangeTableIndexCell = NULL;

	ExtractRangeTableIndexWalker((Node *) setOperation, &rangeTableIndexList);

	foreach(rangeTableIndexCell, rangeTableIndexList)
	{
		int rangeTableIndex = lfirst_int(rangeTableIndexCell);

		varToBeAdded->varno = rangeTableIndex;
		AddToAttributeEquivalenceClass(attributeEquivalenceClass, root, varToBeAdded);
	}
}


/*
 * AddRteRelationToAttributeEquivalenceClass adds the given var to the given equivalence
 * class using the rteIdentity provided by the rangeTableEntry. Note that
 * rteIdentities are only assigned to RTE_RELATIONs and this function asserts
 * the input rte to be an RTE_RELATION.
 */
static void
AddRteRelationToAttributeEquivalenceClass(AttributeEquivalenceClass **
										  attrEquivalenceClass,
										  RangeTblEntry *rangeTableEntry,
										  Var *varToBeAdded)
{
	AttributeEquivalenceClassMember *attributeEqMember = NULL;
	Oid relationId = rangeTableEntry->relid;
	Var *relationPartitionKey = DistPartitionKey(relationId);

	Assert(rangeTableEntry->rtekind == RTE_RELATION);

	if (PartitionMethod(relationId) != DISTRIBUTE_BY_NONE &&
		relationPartitionKey->varattno != varToBeAdded->varattno)
	{
		return;
	}

	attributeEqMember = palloc0(sizeof(AttributeEquivalenceClassMember));

	attributeEqMember->varattno = varToBeAdded->varattno;
	attributeEqMember->varno = varToBeAdded->varno;
	attributeEqMember->rteIdentity = GetRTEIdentity(rangeTableEntry);
	attributeEqMember->relationId = rangeTableEntry->relid;

	(*attrEquivalenceClass)->equivalentAttributes =
		lappend((*attrEquivalenceClass)->equivalentAttributes,
				attributeEqMember);
}


/*
 * AttributeClassContainsAttributeClassMember returns true if it the input class member
 * is already exists in the attributeEquivalenceClass. An equality is identified by the
 * varattno and rteIdentity.
 */
static bool
AttributeClassContainsAttributeClassMember(AttributeEquivalenceClassMember *inputMember,
										   AttributeEquivalenceClass *
										   attributeEquivalenceClass)
{
	ListCell *classCell = NULL;
	foreach(classCell, attributeEquivalenceClass->equivalentAttributes)
	{
		AttributeEquivalenceClassMember *memberOfClass =
			(AttributeEquivalenceClassMember *) lfirst(classCell);
		if (memberOfClass->rteIdentity == inputMember->rteIdentity &&
			memberOfClass->varattno == inputMember->varattno)
		{
			return true;
		}
	}

	return false;
}


/*
 * AddAttributeClassToAttributeClassList checks for certain properties of the
 * input attributeEquivalance before adding it to the attributeEquivalenceList.
 *
 * Firstly, the function skips adding NULL attributeEquivalance to the list.
 * Secondly, since an attribute equivalence class with a single member does
 * not contribute to our purposes, we skip such classed adding to the list.
 * Finally, we don't want to add an equivalence class whose exact equivalent
 * already exists in the list.
 */
static List *
AddAttributeClassToAttributeClassList(List *attributeEquivalenceList,
									  AttributeEquivalenceClass *attributeEquivalance)
{
	List *equivalentAttributes = NULL;
	ListCell *attributeEquivalanceCell = NULL;

	if (attributeEquivalance == NULL)
	{
		return attributeEquivalenceList;
	}

	/*
	 * Note that in some cases we allow having equivalentAttributes with zero or
	 * one elements. For the details, see AddToAttributeEquivalenceClass().
	 */
	equivalentAttributes = attributeEquivalance->equivalentAttributes;
	if (list_length(equivalentAttributes) < 2)
	{
		return attributeEquivalenceList;
	}

	/* we don't want to add an attributeEquivalance which already exists */
	foreach(attributeEquivalanceCell, attributeEquivalenceList)
	{
		AttributeEquivalenceClass *currentAttributeEquivalance =
			(AttributeEquivalenceClass *) lfirst(attributeEquivalanceCell);

		if (AttributeEquivalancesAreEqual(currentAttributeEquivalance,
										  attributeEquivalance))
		{
			return attributeEquivalenceList;
		}
	}

	attributeEquivalenceList = lappend(attributeEquivalenceList,
									   attributeEquivalance);

	return attributeEquivalenceList;
}


/*
 *  AttributeEquivalancesAreEqual returns true if both input attribute equivalence
 *  classes contains exactly the same members.
 */
static bool
AttributeEquivalancesAreEqual(AttributeEquivalenceClass *firstAttributeEquivalance,
							  AttributeEquivalenceClass *secondAttributeEquivalance)
{
	List *firstEquivalenceMemberList =
		firstAttributeEquivalance->equivalentAttributes;
	List *secondEquivalenceMemberList =
		secondAttributeEquivalance->equivalentAttributes;
	ListCell *firstAttributeEquivalanceCell = NULL;
	ListCell *secondAttributeEquivalanceCell = NULL;

	if (list_length(firstEquivalenceMemberList) != list_length(
			secondEquivalenceMemberList))
	{
		return false;
	}

	foreach(firstAttributeEquivalanceCell, firstEquivalenceMemberList)
	{
		AttributeEquivalenceClassMember *firstEqMember =
			(AttributeEquivalenceClassMember *) lfirst(firstAttributeEquivalanceCell);
		bool foundAnEquivalentMember = false;

		foreach(secondAttributeEquivalanceCell, secondEquivalenceMemberList)
		{
			AttributeEquivalenceClassMember *secondEqMember =
				(AttributeEquivalenceClassMember *) lfirst(
					secondAttributeEquivalanceCell);

			if (firstEqMember->rteIdentity == secondEqMember->rteIdentity &&
				firstEqMember->varattno == secondEqMember->varattno)
			{
				foundAnEquivalentMember = true;
				break;
			}
		}

		/* we couldn't find an equivalent member */
		if (!foundAnEquivalentMember)
		{
			return false;
		}
	}

	return true;
}


/*
 * ContainsUnionSubquery gets a queryTree and returns true if the query
 * contains
 *      - a subquery with UNION set operation
 *      - no joins above the UNION set operation in the query tree
 *
 * Note that the function allows top level unions being wrapped into aggregations
 * queries and/or simple projection queries that only selects some fields from
 * the lower level queries.
 *
 * If there exists joins before the set operations, the function returns false.
 * Similarly, if the query does not contain any union set operations, the
 * function returns false.
 */
bool
ContainsUnionSubquery(Query *queryTree)
{
	List *rangeTableList = queryTree->rtable;
	Node *setOperations = queryTree->setOperations;
	List *joinTreeTableIndexList = NIL;
	Index subqueryRteIndex = 0;
	uint32 joiningRangeTableCount = 0;
	RangeTblEntry *rangeTableEntry = NULL;
	Query *subqueryTree = NULL;

	ExtractRangeTableIndexWalker((Node *) queryTree->jointree, &joinTreeTableIndexList);
	joiningRangeTableCount = list_length(joinTreeTableIndexList);

	/* don't allow joins on top of unions */
	if (joiningRangeTableCount > 1)
	{
		return false;
	}

	subqueryRteIndex = linitial_int(joinTreeTableIndexList);
	rangeTableEntry = rt_fetch(subqueryRteIndex, rangeTableList);
	if (rangeTableEntry->rtekind != RTE_SUBQUERY)
	{
		return false;
	}

	subqueryTree = rangeTableEntry->subquery;
	setOperations = subqueryTree->setOperations;
	if (setOperations != NULL)
	{
		SetOperationStmt *setOperationStatement = (SetOperationStmt *) setOperations;

		/*
		 * Note that the set operation tree is traversed elsewhere for ensuring
		 * that we only support UNIONs.
		 */
		if (setOperationStatement->op != SETOP_UNION)
		{
			return false;
		}

		return true;
	}

	return ContainsUnionSubquery(subqueryTree);
}


/*
 * RelationRestrictionPartitionKeyIndex gets a relation restriction and finds the
 * index that the partition key of the relation exists in the query. The query is
 * found in the planner info of the relation restriction.
 */
static Index
RelationRestrictionPartitionKeyIndex(RelationRestriction *relationRestriction)
{
	PlannerInfo *relationPlannerRoot = NULL;
	Query *relationPlannerParseQuery = NULL;
	List *relationTargetList = NIL;
	ListCell *targetEntryCell = NULL;
	Index partitionKeyTargetAttrIndex = 0;

	relationPlannerRoot = relationRestriction->plannerInfo;
	relationPlannerParseQuery = relationPlannerRoot->parse;
	relationTargetList = relationPlannerParseQuery->targetList;

	foreach(targetEntryCell, relationTargetList)
	{
		TargetEntry *targetEntry = (TargetEntry *) lfirst(targetEntryCell);
		Expr *targetExpression = targetEntry->expr;

		partitionKeyTargetAttrIndex++;

		if (!targetEntry->resjunk &&
			IsPartitionColumn(targetExpression, relationPlannerParseQuery) &&
			IsA(targetExpression, Var))
		{
			Var *targetColumn = (Var *) targetExpression;

			if (targetColumn->varno == relationRestriction->index)
			{
				return partitionKeyTargetAttrIndex;
			}
		}
	}

	return InvalidAttrNumber;
}


/*
 * RelationIdList returns list of unique relation ids in query tree.
 */
List *
RelationIdList(Query *query)
{
	List *rangeTableList = NIL;
	List *tableEntryList = NIL;
	List *relationIdList = NIL;
	ListCell *tableEntryCell = NULL;

	ExtractRangeTableRelationWalker((Node *) query, &rangeTableList);
	tableEntryList = TableEntryList(rangeTableList);

	foreach(tableEntryCell, tableEntryList)
	{
		TableEntry *tableEntry = (TableEntry *) lfirst(tableEntryCell);
		Oid relationId = tableEntry->relationId;

		relationIdList = list_append_unique_oid(relationIdList, relationId);
	}

	return relationIdList;
}
