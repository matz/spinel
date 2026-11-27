# A program that reopens Object (a core extension every class inherits) and
# uses the sqlite3 library. Database's `alias_method :busy_timeout,
# :busy_timeout=` is then an alias in a class with a parent, whose reading
# asks where the aliased method was defined: in sqlite3_native.rb, which the
# library required by a path computed from RUBY_VERSION under a rescue, a
# file at no fixed point of the load order, and the alias was refused.
class Object
  def blank? = respond_to?(:empty?) ? !!empty? : !self
end
require "sqlite3"
db = SQLite3::Database.new(":memory:")
db.busy_timeout = 100
db.busy_timeout(50)
db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)")
db.execute("INSERT INTO users (name) VALUES ('alice')")
p db.execute("SELECT id, name FROM users"), "".blank?, [1].blank?
