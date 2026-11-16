# A send beside a top-level def of the name, on a value of one of several
# classes, takes the dispatch for the values the dispatch's arms take and the
# top-level def for the rest. A method the program adds to Numeric answers an
# Integer and a Float; a String, which no arm takes, reaches the top-level def
# and runs it, where it was sent into the dispatch and raised NoMethodError.
class Numeric
  def kind = self.class
  def same = :numeric_same
end
def kind = :top
def same = :top_same
[1, 2.5, "s"].each { |o| p o.send(:kind) }
["s", 3, 1].each { |o| p o.send(:same) }
[2.5, "s", 2].each { |o| p o.__send__(:same) }
[2.5, "s", 3].each { |o| p o.send(:same) }

# A reopened Integer's method answers an Integer; a Float beside it reaches
# the top-level def.
class Integer
  def twice = self * 2
end
def twice = :top_twice
[3, 2.5].each { |o| p o.send(:twice) }
