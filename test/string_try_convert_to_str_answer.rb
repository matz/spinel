# String.try_convert checks what #to_str answers: nil and a String are the
# result, any other answer is a TypeError.
class B; def to_str = 5; end
class E; def to_str = nil; end
class F; def to_str = (rand > 2 ? 1 : nil); end
class G; def to_str = (rand > 2 ? 1 : "g"); end
class H; private def to_str = "h"; end
class I < B; end
class S; def to_str = 1.5; end
class T; def to_str = [1]; end
class U; def to_str = "u"; end

p((String.try_convert(B.new) rescue $!.message))
p String.try_convert(E.new)
p String.try_convert(F.new)
p String.try_convert(G.new)
p String.try_convert(H.new)
p((String.try_convert(I.new) rescue $!.message))
p((String.try_convert(S.new) rescue $!.class))
p((String.try_convert(T.new) rescue $!.message))
p String.try_convert(U.new)
p String.try_convert("plain")
p String.try_convert(3)
