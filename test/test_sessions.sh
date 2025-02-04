#! /bin/bash

export HOME="./sessions"
unset XDG_CONFIG_HOME
rm -rf "./sessions"
mkdir -p $HOME

run_cap_test ${lnav_test} -n \
    -c ":reset-session" \
    -c ":goto 0" \
    -c ":hide-file" \
    -c ":save-session" \
    ${test_dir}/logfile_access_log.*

# hidden file saved in session
run_cap_test ${lnav_test} -n \
    -c ":load-session" \
    ${test_dir}/logfile_access_log.*

# setting log_mark
run_cap_test ${lnav_test} -nq \
    -c ":reset-session" \
    -c ";update access_log set log_mark = 1 where sc_bytes > 60000" \
    -c ":goto 1" \
    -c ":partition-name middle" \
    -c ":save-session" \
    ${test_dir}/logfile_access_log.0

# log mark was not saved in session
run_cap_test ${lnav_test} -n \
    -c ":load-session" \
    -c ':write-to -' \
    ${test_dir}/logfile_access_log.0

# file was not closed
run_cap_test ${lnav_test} -n \
    -c ":load-session" \
    -c ":close" \
    -c ":save-session" \
    ${test_dir}/logfile_access_log.0

# partition name was not saved in session
run_cap_test ${lnav_test} -n \
    -c ":load-session" \
    -c ';select log_line,log_part from access_log' \
    -c ':write-csv-to -' \
    ${test_dir}/logfile_access_log.0

# adjust time is not working
run_cap_test ${lnav_test} -nq \
    -c ":adjust-log-time 2010-01-01T00:00:00" \
    -c ":save-session" \
    ${test_dir}/logfile_access_log.0

# adjust time is not saved in session
run_cap_test ${lnav_test} -n \
    -c ":load-session" \
    -c ":test-comment adjust time in session" \
    ${test_dir}/logfile_access_log.0

# hiding fields failed
rm -rf ./sessions
mkdir -p $HOME
run_cap_test ${lnav_test} -nq -d /tmp/lnav.err \
    -c ":hide-fields c_ip" \
    -c ":save-session" \
    ${test_dir}/logfile_access_log.0

# restoring hidden fields failed
run_cap_test ${lnav_test} -n \
    -c ":load-session" \
    -c ":test-comment restoring hidden fields" \
    ${test_dir}/logfile_access_log.0

# hiding fields failed
rm -rf ./sessions
mkdir -p $HOME
run_cap_test ${lnav_test} -nq -d /tmp/lnav.err \
    -c ":hide-lines-before 2009-07-20 22:59:29" \
    -c ":save-session" \
    ${test_dir}/logfile_access_log.0

# XXX we don't actually check
# restoring hidden fields failed
run_cap_test ${lnav_test} -n -d /tmp/lnav.err \
    -c ":load-session" \
    -c ":test-comment restore hidden lines" \
    ${test_dir}/logfile_access_log.0
