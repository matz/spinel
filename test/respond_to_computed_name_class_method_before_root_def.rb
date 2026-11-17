# A name a later Object reopening defines is left out of a computed-name respond_to?, unless a class of the
# program answers it itself: its public class method (its own, a superclass's, an extended module's) answers
# where it is defined, before the reopening runs.
class Pub
  def self.later = 1
end
class Sub < Pub; end
module Ext
  def later = 3
end
class Via
  extend Ext
end
name = ARGV[0] ? "other" : "later"
[Pub, Sub, Via].each do |v|
  puts [v.respond_to?(name), v.respond_to?(name, true)].inspect
end
class Object
  def later = 4
end
[Pub, Sub, Via].each do |v|
  puts [v.respond_to?(name), v.respond_to?(name, true)].inspect
end
