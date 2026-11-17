# The initialize a reopening above a builtin exception gives it, run from `.new`
# and the raise forms (exception_builtin_below_reopen_initialize.rb), in three
# shapes it first got wrong:
#   - a default that reads `self` (`msg = self.class.name`) reads the exception
#     being built, also when the construction sits in another call's arguments;
#   - the call's block goes to the initialize's `&blk`;
#   - a class whose own initialize takes more than the message (NameError's
#     name, KeyError's key: and receiver:) keeps them: they are read natively,
#     and the reopening is left out for such a call (only their attributes are
#     checked here), where it had made `NameError.new("n", :field)` raise an
#     ArgumentError and left the key and receiver nil.
class StandardError
  def initialize(msg = self.class.name, &block)
    @blk = block
    super("S[#{msg}]")
  end
  def blk? = !@blk.nil?
end
class Box
  def initialize(e) = @e = e
  def msg = @e.message
end
p ArgumentError.new.message, TypeError.new.message
p Box.new(ArgumentError.new).msg
p [TypeError.new.message, IOError.new("x").message]
begin; raise ArgumentError; rescue => e; p e.message; end
begin; raise RangeError, "r"; rescue => e; p e.message, e.blk?; end

p ArgumentError.new("a").blk?
p ArgumentError.new("a") { 1 }.blk?
p IndexError.new("i") { 2 }.blk?

n = NameError.new("n", :field)
p n.name
k = KeyError.new("k", receiver: 1, key: :kk)
p k.key, k.receiver
