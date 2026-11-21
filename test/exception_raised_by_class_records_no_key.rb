# `raise KeyError, "m"` (NameError, FrozenError) builds the error the way
# `KeyError.new("m")` does: it records no key and no receiver, so #key and
# #receiver raise ArgumentError, as in CRuby. The errors the runtime raises
# (Hash#fetch, a missing method, a frozen String) still carry theirs.

def key_of(e) = (begin; e.key.inspect; rescue ArgumentError => x; x.message; end)
def receiver_of(e) = (begin; e.receiver.inspect; rescue ArgumentError => x; x.message; end)

begin; raise KeyError, "m"; rescue KeyError => e; p [key_of(e), receiver_of(e), e.message]; end
msg = "built " + "at run time"
begin; raise KeyError, msg; rescue KeyError => e; p [key_of(e), receiver_of(e), e.message]; end
begin; raise KeyError; rescue KeyError => e; p [key_of(e), receiver_of(e), e.message]; end
begin; raise NameError, "m"; rescue NameError => e; p [receiver_of(e), e.name]; end
begin; raise NoMethodError; rescue NameError => e; p [receiver_of(e), e.name]; end
begin; raise FrozenError, "m"; rescue FrozenError => e; p receiver_of(e); end
begin; raise KeyError, nil; rescue KeyError => e; p [key_of(e), e.message]; end
begin; raise KeyError, :sym; rescue KeyError => e; p [key_of(e), e.message]; end
held = { a: "held message" }
begin; raise KeyError, held[:a]; rescue KeyError => e; p [key_of(e), e.message]; end
begin; raise NameError, 7; rescue NameError => e; p [receiver_of(e), e.message]; end
begin; raise FrozenError, nil; rescue FrozenError => e; p receiver_of(e); end
class OwnKeyError < KeyError; end
begin; raise OwnKeyError, "m"; rescue => e; p [key_of(e), receiver_of(e)]; end
begin; raise KeyError.new("m", receiver: {}, key: :k); rescue KeyError => e; p [key_of(e), receiver_of(e)]; end

begin; {}.fetch(:a); rescue KeyError => e; p [key_of(e), receiver_of(e)]; end
begin; nil.zork; rescue NoMethodError => e; p [receiver_of(e), e.name]; end
begin; "a".dup.freeze << "b"; rescue FrozenError => e; p receiver_of(e); end
begin
  begin; {}.fetch(:again); rescue KeyError; raise; end
rescue KeyError => e
  p key_of(e)
end
