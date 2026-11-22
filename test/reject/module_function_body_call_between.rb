# spinel: reject-subclass: where more than one def of the method may be live
# A method body's module_function copies the def live where the word runs. A
# call between the body's def and its word can define the method again (CRuby
# copies `def m = 2`: M.m answers 2), so the def written ahead of the word is
# not the one the word finds, and where the record cannot tell the live def the
# program is refused rather than copied from the stale one.
module M
  def self.change
    def m = 2
  end
  def self.setup
    def m = 1
    change
    module_function :m
  end
end
M.setup
p M.m
