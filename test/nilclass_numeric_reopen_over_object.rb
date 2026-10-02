# A NilClass or Numeric reopen answers for nil and for an Integer or Float
# receiver ahead of an Object reopen of the same name, as CRuby's ancestry
# has it. The call was typed from Object's method while the emitter called
# the reopen's, so a Float answer landed in a String slot and the C did not
# compile.
class Object
  def kind_tag = "object"
  def shared = :object
end

class NilClass
  def kind_tag = 0
end

class Numeric
  def kind_tag = 1.5
end

x = nil
p x.kind_tag
p nil.kind_tag
p 3.kind_tag
p 2.5.kind_tag
p "s".kind_tag
p [1].kind_tag
p 7.shared
p nil.shared
