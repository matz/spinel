# param_chain_boxed_after_integer.rb over thirty methods, the last of which
# stores the value: the instance variable and its reader follow the
# widened parameters.
class Keep
  def initialize = @held = 0
  def hold(value) = @held = value
  def held = @held
end
KEEP = Keep.new
def far0(value) = KEEP.hold(value)
def far1(value) = far0(value)
def far2(value) = far1(value)
def far3(value) = far2(value)
def far4(value) = far3(value)
def far5(value) = far4(value)
def far6(value) = far5(value)
def far7(value) = far6(value)
def far8(value) = far7(value)
def far9(value) = far8(value)
def far10(value) = far9(value)
def far11(value) = far10(value)
def far12(value) = far11(value)
def far13(value) = far12(value)
def far14(value) = far13(value)
def far15(value) = far14(value)
def far16(value) = far15(value)
def far17(value) = far16(value)
def far18(value) = far17(value)
def far19(value) = far18(value)
def far20(value) = far19(value)
def far21(value) = far20(value)
def far22(value) = far21(value)
def far23(value) = far22(value)
def far24(value) = far23(value)
def far25(value) = far24(value)
def far26(value) = far25(value)
def far27(value) = far26(value)
def far28(value) = far27(value)
def far29(value) = far28(value)
def far30(value) = far29(value)
def over(v) = far30(v)
p far30(0)
p over(2.5)
p KEEP.held
p over("t")
p KEEP.held
p far30(3) + 1
