#
# How we organize this isolation test spec, is explained at README.md file in this directory.
#

# create range distributed table to test behavior of TRUNCATE in concurrent operations
setup
{
	SET citus.shard_replication_factor TO 1;
	CREATE TABLE truncate_hash(id integer, data text);
	SELECT create_distributed_table('truncate_hash', 'id');
}

# drop distributed table
teardown
{
	DROP TABLE IF EXISTS truncate_hash CASCADE;

	SELECT citus.restore_isolation_tester_func();
}

# session 1
session "s1"
step "s1-initialize" { COPY truncate_hash FROM PROGRAM 'echo 0, a\\n1, b\\n2, c\\n3, d\\n4, e' WITH CSV; }
step "s1-begin" { BEGIN; }
step "s1-truncate" { TRUNCATE truncate_hash; }
step "s1-drop" { DROP TABLE truncate_hash; }
step "s1-ddl-create-index" { CREATE INDEX truncate_hash_index ON truncate_hash(id); }
step "s1-ddl-drop-index" { DROP INDEX truncate_hash_index; }
step "s1-ddl-add-column" { ALTER TABLE truncate_hash ADD new_column int DEFAULT 0; }
step "s1-ddl-drop-column" { ALTER TABLE truncate_hash DROP new_column; }
step "s1-ddl-rename-column" { ALTER TABLE truncate_hash RENAME data TO new_data; }
step "s1-table-size" { SELECT citus_total_relation_size('truncate_hash'); }
step "s1-master-modify-multiple-shards" { SELECT master_modify_multiple_shards('TRUNCATE FROM truncate_hash;'); }
step "s1-create-non-distributed-table" { CREATE TABLE truncate_hash(id integer, data text); COPY truncate_hash FROM PROGRAM 'echo 0, a\\n1, b\\n2, c\\n3, d\\n4, e' WITH CSV; }
step "s1-distribute-table" { SELECT create_distributed_table('truncate_hash', 'id'); }
step "s1-select-count" { SELECT COUNT(*) FROM truncate_hash; }
step "s1-commit" { COMMIT; }

# session 2
session "s2"
step "s2-begin" { BEGIN; }
step "s2-truncate" { TRUNCATE truncate_hash; }
step "s2-drop" { DROP TABLE truncate_hash; }
step "s2-ddl-create-index" { CREATE INDEX truncate_hash_index ON truncate_hash(id); }
step "s2-ddl-drop-index" { DROP INDEX truncate_hash_index; }
step "s2-ddl-create-index-concurrently" { CREATE INDEX CONCURRENTLY truncate_hash_index ON truncate_hash(id); }
step "s2-ddl-add-column" { ALTER TABLE truncate_hash ADD new_column int DEFAULT 0; }
step "s2-ddl-drop-column" { ALTER TABLE truncate_hash DROP new_column; }
step "s2-ddl-rename-column" { ALTER TABLE truncate_hash RENAME data TO new_data; }
step "s2-table-size" { SELECT citus_total_relation_size('truncate_hash'); }
step "s2-master-modify-multiple-shards" { SELECT master_modify_multiple_shards('TRUNCATE FROM truncate_hash;'); }
step "s2-create-non-distributed-table" { CREATE TABLE truncate_hash(id integer, data text); COPY truncate_hash FROM PROGRAM 'echo 0, a\\n1, b\\n2, c\\n3, d\\n4, e' WITH CSV; }
step "s2-distribute-table" { SELECT create_distributed_table('truncate_hash', 'id'); }
step "s2-select" { SELECT * FROM truncate_hash ORDER BY id, data; }
step "s2-commit" { COMMIT; }

# permutations - TRUNCATE vs TRUNCATE
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"

# permutations - TRUNCATE first
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-drop" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-ddl-create-index" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-ddl-create-index" "s1-begin" "s2-begin" "s1-truncate" "s2-ddl-drop-index" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-truncate" "s2-ddl-create-index-concurrently" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-ddl-add-column" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-ddl-add-column" "s1-begin" "s2-begin" "s1-truncate" "s2-ddl-drop-column" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-table-size" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-master-modify-multiple-shards" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-drop" "s1-create-non-distributed-table" "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-distribute-table" "s1-commit" "s2-commit" "s1-select-count"

# permutations - TRUNCATE second
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-truncate" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-drop" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-ddl-create-index" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-ddl-create-index" "s1-begin" "s2-begin" "s1-ddl-drop-index" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-ddl-add-column" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-ddl-add-column" "s1-begin" "s2-begin" "s1-ddl-drop-column" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-table-size" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s2-begin" "s1-master-modify-multiple-shards" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
permutation "s1-drop" "s1-create-non-distributed-table" "s1-initialize" "s1-begin" "s2-begin" "s1-distribute-table" "s2-truncate" "s1-commit" "s2-commit" "s1-select-count"
