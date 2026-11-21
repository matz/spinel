# A KeyError, NameError or FrozenError built with extra arguments hands StandardError#initialize what
# CRuby's own initialize leaves for its super: no argument when the call gave no message, nil when it gave
# nil, and the call's block either way.
class StandardError
  def initialize(*a, &b)
    $blk = b ? b.call : :none
    super("S#{a.inspect}")
  end
end

p KeyError.new(key: 1).message
p KeyError.new(nil, key: 1).message
p KeyError.new("k", key: 1).message
p NameError.new.message
p NameError.new(nil, :f).message
p NameError.new("n", :f).message
p FrozenError.new(receiver: 5).message
p FrozenError.new(nil, receiver: 5).message
p FrozenError.new("f", receiver: 5).message

KeyError.new("k", key: 1) { :given }
p $blk
NameError.new("n", :f) { :named }
p $blk
KeyError.new("k", key: 1)
p $blk
e = KeyError.new(key: 7)
p e.key
p e.receiver rescue p $!.class
