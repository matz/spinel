# An operator write of an instance variable or a class variable (`@x **= 1`)
# calls its operator on the value. The load record asks for it the way it asks
# for a call's name, so a class method that does one with a method that
# defines, called while a class body runs, is code it cannot follow: the span
# of the class method's versions over the call is unknown (--dump-load-record,
# record_variable_op_write_main.rb.load-record, "seen="). A global's setter
# (`$g = 1`) may call back as well (`$stdout = w` asks w respond_to?), so a
# write of one is code it cannot follow. A write of an instance variable alone
# (Pl) is not, and its span is the first def. An operator write of a local, or
# through a call or an index, is answered as before.
class N1
  def **(o) = (class << self; def w = 1; end; self)
end
class Iv
  def self.v = 1
  @x = N1.new
  def self.bump = (@x **= 1; 0)
  bump
end
def Iv.v = 2

class Gv
  def self.v = 1
  def self.bump = ($g = 1; 0)
  bump
end
def Gv.v = 2

class Pl
  def self.v = 1
  def self.bump = (@y = 1; 0)
  bump
end
def Pl.v = 2
