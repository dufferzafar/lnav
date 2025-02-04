#! /bin/bash

run_cap_test ./drive_sql "select length(gzip(1))"

run_cap_test ./drive_sql "select gunzip(gzip(1))"

run_cap_test ./drive_sql "select humanize_file_size()"

run_cap_test ./drive_sql "select humanize_file_size('abc')"

run_cap_test ./drive_sql "select humanize_file_size(1, 2)"

run_cap_test ./drive_sql "select humanize_file_size(10 * 1000 * 1000)"

run_cap_test ./drive_sql "select startswith('.foo', '.')"

run_cap_test ./drive_sql "select startswith('foo', '.')"

run_cap_test ./drive_sql "select endswith('foo', '.')"

run_cap_test ./drive_sql "select endswith('foo.', '.')"

run_cap_test ./drive_sql "select endswith('foo.txt', '.txt')"

run_cap_test ./drive_sql "select endswith('a', '.txt')"

run_cap_test ./drive_sql "select regexp('abcd', 'abcd')"

run_cap_test ./drive_sql "select regexp('bc', 'abcd')"

run_cap_test ./drive_sql "select regexp('[e-z]+', 'abcd')"

run_cap_test ./drive_sql "select regexp('[e-z]+', 'ea')"

run_cap_test ./drive_sql "select regexp_replace('test 1 2 3', '\\d+', 'N')"

run_cap_test env TEST_COMMENT=regexp_replace_with_bs1 ./drive_sql <<'EOF'
select regexp_replace('test 1 2 3', '\s+', '{\0}') as repl
EOF

run_cap_test env TEST_COMMENT=regexp_replace_with_bs2 ./drive_sql <<'EOF'
select regexp_replace('test 1 2 3', '\w*', '{\0}') as repl
EOF

run_cap_test ./drive_sql "select regexp_replace('123 abc', '(\w*)', '<\3>') as repl"

run_cap_test env TEST_COMMENT=regexp_replace_with_bs3 ./drive_sql <<'EOF'
select regexp_replace('123 abc', '(\w*)', '<\\>') as repl
EOF

run_cap_test ./drive_sql "select regexp_replace('abc: def', '(\w*):\s*(.*)', '\1=\2') as repl"

run_cap_test ./drive_sql "select regexp_match('abc', 'abc')"

run_cap_test ./drive_sql "select regexp_match(null, 'abc')"

run_cap_test ./drive_sql "select regexp_match('abc', null) as result"

run_cap_test ./drive_sql "select typeof(result), result from (select regexp_match('(\d*)abc', 'abc') as result)"

run_cap_test ./drive_sql "select typeof(result), result from (select regexp_match('(\d*)abc(\d*)', 'abc') as result)"

run_cap_test ./drive_sql "select typeof(result), result from (select regexp_match('(\d+)', '123') as result)"

run_cap_test ./drive_sql "select typeof(result), result from (select regexp_match('a(\d+\.\d+)a', 'a123.456a') as result)"

run_cap_test ./drive_sql "select regexp_match('foo=(?<foo>\w+); (\w+)', 'foo=abc; 123') as result"

run_cap_test ./drive_sql "select regexp_match('foo=(?<foo>\w+); (\w+\.\w+)', 'foo=abc; 123.456') as result"

run_cap_test ./drive_sql "select extract('foo=1') as result"

run_cap_test ./drive_sql "select extract('foo=1; bar=2') as result"

run_cap_test ./drive_sql "select extract(null) as result"

run_cap_test ./drive_sql "select extract(1) as result"

run_cap_test ./drive_sql "select logfmt2json('foo=1 bar=2 baz=2e1 msg=hello') as result"

run_cap_test ./drive_sql "SELECT substr('#foo', range_start) AS value FROM regexp_capture('#foo', '(\w+)') WHERE capture_index = 1"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture('foo bar', '\w+ (\w+)')"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture('foo bar', '\w+ \w+')"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture('foo bar', '\w+ (?<word>\w+)')"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture('foo bar', '(bar)|\w+ (?<word>\w+)')"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture()"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture('foo bar')"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture('foo bar', '(')"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture('1 2 3 45', '(\d+)')"

run_cap_test ./drive_sql "SELECT * FROM regexp_capture('foo foo', '^foo')"
