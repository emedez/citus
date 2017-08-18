#
# How we organize this isolation test spec, is explained at README.md file in this directory.
#

# create range distributed table to test behavior of INSERT/SELECT in concurrent operations
setup
{
	SET citus.shard_replication_factor TO 1;
	CREATE TABLE insert_select_append(id integer, data text);
	SELECT create_distributed_table('insert_select_append', 'id', 'append');
}

# drop distributed table
teardown
{
	DROP TABLE IF EXISTS insert_select_append CASCADE;
}

# session 1
session "s1"
step "s1-initialize" { COPY insert_select_append FROM PROGRAM 'echo 0, a\\n1, b\\n2, c\\n3, d\\n4, e' WITH CSV; }
step "s1-begin" { BEGIN; }
step "s1-insert-select" { INSERT INTO insert_select_append SELECT * FROM insert_select_append; }
step "s1-update" { UPDATE insert_select_append SET data = 'l' WHERE id = 4; }
step "s1-delete" { DELETE FROM insert_select_append WHERE id = 4; }
step "s1-truncate" { TRUNCATE insert_select_append; }
step "s1-drop" { DROP TABLE insert_select_append; }
step "s1-ddl-create-index" { CREATE INDEX insert_select_append_index ON insert_select_append(id); }
step "s1-ddl-drop-index" { DROP INDEX insert_select_append_index; }
step "s1-ddl-add-column" { ALTER TABLE insert_select_append ADD new_column int DEFAULT 0; }
step "s1-ddl-drop-column" { ALTER TABLE insert_select_append DROP new_column; }
step "s1-ddl-rename-column" { ALTER TABLE insert_select_append RENAME data TO new_data; }
step "s1-table-size" { SELECT citus_total_relation_size('insert_select_append'); }
step "s1-master-modify-multiple-shards" { SELECT master_modify_multiple_shards('DELETE FROM insert_select_append;'); }
step "s1-master-apply-delete-command" { SELECT master_apply_delete_command('DELETE FROM insert_select_append WHERE id <= 4;'); }
step "s1-master-drop-all-shards" { SELECT master_drop_all_shards('insert_select_append'::regclass, 'public', 'insert_select_append'); }
step "s1-create-non-distributed-table" { CREATE TABLE insert_select_append(id integer, data text); }
step "s1-distribute-table" { SELECT create_distributed_table('insert_select_append', 'id', 'append'); }
step "s1-select-count" { SELECT COUNT(*) FROM insert_select_append; }
step "s1-commit" { COMMIT; }

# session 2
session "s2"
step "s2-insert-select" { INSERT INTO insert_select_append SELECT * FROM insert_select_append; }
step "s2-update" { UPDATE insert_select_append SET data = 'l' WHERE id = 4; }
step "s2-delete" { DELETE FROM insert_select_append WHERE id = 4; }
step "s2-truncate" { TRUNCATE insert_select_append; }
step "s2-drop" { DROP TABLE insert_select_append; }
step "s2-ddl-create-index" { CREATE INDEX insert_select_append_index ON insert_select_append(id); }
step "s2-ddl-drop-index" { DROP INDEX insert_select_append_index; }
step "s2-ddl-create-index-concurrently" { CREATE INDEX CONCURRENTLY insert_select_append_index ON insert_select_append(id); }
step "s2-ddl-add-column" { ALTER TABLE insert_select_append ADD new_column int DEFAULT 0; }
step "s2-ddl-drop-column" { ALTER TABLE insert_select_append DROP new_column; }
step "s2-ddl-rename-column" { ALTER TABLE insert_select_append RENAME data TO new_data; }
step "s2-table-size" { SELECT citus_total_relation_size('insert_select_append'); }
step "s2-master-modify-multiple-shards" { SELECT master_modify_multiple_shards('DELETE FROM insert_select_append;'); }
step "s2-master-apply-delete-command" { SELECT master_apply_delete_command('DELETE FROM insert_select_append WHERE id <= 4;'); }
step "s2-master-drop-all-shards" { SELECT master_drop_all_shards('insert_select_append'::regclass, 'public', 'insert_select_append'); }
step "s2-create-non-distributed-table" { CREATE TABLE insert_select_append(id integer, data text); }
step "s2-distribute-table" { SELECT create_distributed_table('insert_select_append', 'id'); }
step "s2-select" { SELECT * FROM insert_select_append ORDER BY id, data; }

# permutations - INSERT/SELECT vs INSERT/SELECT
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-insert-select" "s1-commit" "s1-select-count"

# permutations - INSERT/SELECT first
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-update" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-delete" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-truncate" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-drop" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-ddl-create-index" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-ddl-create-index" "s1-begin" "s1-insert-select" "s2-ddl-drop-index" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-ddl-create-index-concurrently" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-ddl-add-column" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-ddl-add-column" "s1-begin" "s1-insert-select" "s2-ddl-drop-column" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-table-size" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-master-modify-multiple-shards" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-master-apply-delete-command" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-master-drop-all-shards" "s1-commit" "s1-select-count"
permutation "s1-drop" "s1-create-non-distributed-table" "s1-begin" "s1-insert-select" "s2-distribute-table" "s1-commit" "s1-select-count"

# permutations - INSERT/SELECT second
permutation "s1-initialize" "s1-begin" "s1-insert-select" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-update" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-delete" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-truncate" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-drop" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-ddl-create-index" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-ddl-create-index" "s1-begin" "s1-ddl-drop-index" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-ddl-add-column" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-ddl-add-column" "s1-begin" "s1-ddl-drop-column" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-table-size" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-master-modify-multiple-shards" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-master-apply-delete-command" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-initialize" "s1-begin" "s1-master-drop-all-shards" "s2-insert-select" "s1-commit" "s1-select-count"
permutation "s1-drop" "s1-create-non-distributed-table" "s1-begin" "s1-distribute-table" "s2-insert-select" "s1-commit" "s1-select-count"
