# A rescue clause looks the classes it names up where it stands: at the
# top level, in Object. A class a module defines by the name Exception (as
# sqlite3's errors.rb does) or LoadError is that module's, and a top-level
# reopen of Exception names a class the clause does not list, so none of
# them leaves the clause's run unknown. The fat-binary clause whose
# versioned file exists never runs, and the alias takes that file's
# busy_timeout=, not the fallback's.
module RescueScopedMod
  class Exception < ::StandardError; end
end
RescueScopedMod::LoadError = Class.new(StandardError)
class Exception; end
begin
  require_relative "rescue_scoped_4.0/native_lib"
rescue LoadError
  require_relative "rescue_scoped_native_lib"
end
require_relative "rescue_scoped_db_lib"
p RescueScopedDb.new.busy_timeout(5)
