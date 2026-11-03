# A keyword-init Struct class inspects as `K(keyword_init: true)`, from
# inside its own class method (bare or on self) as from outside, and a call
# of its inspect that runs before the program's class method answers so too
# (see class_builtin_sub_before_override.rb). This file holds the one
# inspect def of its program, as there: a name defined twice keeps the
# table's answer.

class CK < Struct.new(:a, keyword_init: true)
  def self.show = inspect
  def self.show_self = self.inspect
  def self.show_to_s = to_s
end
def early = CK.inspect
def early_of(k) = k.inspect
p CK.show, CK.show_self, CK.show_to_s, early, early_of(CK), CK.inspect

class CK
  def self.inspect = "ck"
end
p early, early_of(CK), CK.show, CK.show_self
