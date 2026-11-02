# A program's own Array.new or Hash.new is not an empty container
# An empty `[]` or `Array.new` arm takes an Integer, Float or String Array
# sibling's kind. A reopened Array whose `new` answers something else is a
# call like any other: its value joins the other arm's as it is.

class Array
  def self.new(*a) = 7
end

class Hash
  def self.new(*a) = "h"
end

c = ARGV.empty?
x = c ? Array.new : [1]
y = c ? [1.5] : Array.new
z = [1] && Array.new
h = c ? Hash.new : {1 => 2}
g = nil || Hash.new
p x, y, z, h, g
