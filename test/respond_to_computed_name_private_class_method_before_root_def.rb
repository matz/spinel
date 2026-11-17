# ...and a private class method of the program's answers only an include_all ask, before a later top-level def of
# the name.
class Priv
  private_class_method def self.later = 2
end
name = ARGV[0] ? "other" : "later"
v = ARGV[0] ? 1 : Priv
puts [v.respond_to?(name), v.respond_to?(name, true)].inspect
def later = 4
puts [v.respond_to?(name), v.respond_to?(name, true)].inspect
