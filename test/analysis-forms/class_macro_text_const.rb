# A class-body macro whose module_eval text writes a constant of a literal
# (#9898): the text's `NAME = <literal>` is its const_set, written where the
# call stands, beside the defs the text makes; a later write in a reopening
# replaces it, and one only the text writes reads as written.
module Consts
  def define_src(name, v) = module_eval("#{name} = #{v.inspect}")
  def define_pair(name, v) = module_eval("#{name} = #{v.inspect}\ndef #{name.downcase} = #{v.inspect}")
end
class C
  extend Consts
  define_src :B, 2
  define_src :S, "str"
  define_pair :K, 7
  p [B, S, K]
end
class C
  B = 6
end
p C::B, C::S, C::K, C.new.k
def read_late = [C::B, C::K]
p read_late
