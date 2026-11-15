# A send of hash or instance_variables on a value of one of several
# classes, beside a top-level def of the name and a reopened Integer, Float or
# Symbol that defines it: the builtin emission of Object's surface does not
# read the reopening, so the send split keeps those values off the dispatch
# and the top-level def answers, which is the class CRuby's answer has for the
# Integer, the Float and the Symbol and for the String.
class Integer
  def hash(*) = :int_hash
end
class Float
  def instance_variables(*) = :flt_ivars
end
class Symbol
  def hash(*) = :sym_hash
end
def hash(*) = :top_hash
def instance_variables(*) = :top_ivars

[3, "s"].each { |o| p((o.send(:hash)).class) }
[3, "s"].each { |o| p((o.__send__(:hash)).class) }
[2.5, "s"].each { |o| p((o.send(:instance_variables)).class) }
[2.5, "s"].each { |o| p((o.__send__(:instance_variables)).class) }
[:a, "s"].each { |o| p((o.send(:hash)).class) }
