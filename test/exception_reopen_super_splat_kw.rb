# A reopened builtin exception's initialize with `super(*a, **k)`: the
# super reaches the builtin's superclass's initialize. NoMethodError's is
# NameError's, which takes receiver:. KeyError's is Exception's, which
# reads one message.
class NoMethodError; def initialize(*a, **k) = super(*a, **k); end
e = NoMethodError.new(*["m"], receiver: 5); p e.message, e.receiver
e = NoMethodError.new(*[], receiver: 6); p e.message, e.receiver
p NoMethodError.new(*["only"]).message
class KeyError; def initialize(*a, **k) = super(*a, **k); end
begin; KeyError.new(*["x"], key: 3); rescue ArgumentError => ex; p ex.message; end
p KeyError.new(*["k"]).message
