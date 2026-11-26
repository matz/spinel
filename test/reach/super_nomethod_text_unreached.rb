# A class method's super that reaches no method raises CRuby's NoMethodError,
# its message built when it is read by sp_super_nomethod_text. In a method
# nothing calls, the super is never written out, so the program has no such
# function.
# absent: sp_super_nomethod_text sp_raise_super_nomethod
class Unused
  def self.nope = super
end
p Unused.name
