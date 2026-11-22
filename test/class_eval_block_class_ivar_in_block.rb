# An instance variable written in a class_eval block of the body's own class
# is the class's own, the one the body and a later block read.
class A
  class << self; attr_reader :lvl, :tags; end
  @lvl = 1
  class_eval do
    @lvl += 1
    @tags = ["a"]
  end
  module_eval { @tags << "b" }
end
p A.lvl, A.tags
