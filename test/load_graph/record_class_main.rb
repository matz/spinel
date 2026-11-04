# A blockless Struct a required file makes is a class of the program at its
# write: the main file gives it a method by class_eval and aliases it.
require_relative "record_class_lib"
Rec.class_eval do
  def tag = "r#{id}"
end
R2 = Rec
p Rec.new(1).tag, R2.new(2).tag, R2.name, Rec.superclass
