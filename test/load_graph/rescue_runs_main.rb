# A `rescue LoadError` clause whose begin's body requires a file the
# program does not have runs whenever the begin completes: that require
# raises LoadError when it is reached (the fat-binary fallback of the
# sqlite3 gem, `require "sqlite3/#{v}/sqlite3_native" rescue LoadError
# require "sqlite3/sqlite3_native"`). The load record holds the clause's
# points as sure, not as may not run, so the definition the clause's file
# makes is in effect, by the record, for an alias a later file makes
# (--dump-load-record, rescue_runs_main.rb.load-record), and the require
# that names no file gets no warning. The alias takes the cext's
# busy_timeout=, not the included module's.
begin
  require_relative "rescue_runs_9.9/native_lib"
rescue LoadError
  require_relative "rescue_runs_native_lib"
end
require_relative "rescue_runs_db_lib"
p RescueRunsDb.new.busy_timeout(5)
p RescueRunsDb.new.send(:busy_timeout=, 6)
