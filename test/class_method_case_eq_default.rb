# A class's own class method `===` with optional parameters after the one
# value (literal or constant defaults) answers `case`/`when`, `in`, grep and
# a class held as a value, as a written `Klass === x` does.
class M2; def self.===(o, extra = 1) = extra == 1 && o == 3; end
case 3; when M2 then p :m2; else p :no; end
case 4; when M2 then p :m2; else p :no; end
class M4 < M2; end
case 3; when M4 then p :m4; else p :no; end
case "s"; when M4 then p :m4; else p :no; end

LIM = 7
class M5; def self.===(o, a = :s, h = LIM, f = 1.5, y = :y, t = true) = [o, a, h, f, y, t]; end
def t(x) = case x; when M5 then :m5; else :no; end
p t(1), t("a"), t(nil)
p [1, "a", :b].grep(M5)
k = [M5, M2].first
p k === 5
p(M5 === 2)

case [1]; in M2 then p :in2; else p :inno; end
class M6; def self.===(o, z = nil) = z.nil? ? o.is_a?(Integer) : false; end
p [1, "x", 2].grep(M6)
case 2; in M6 then p :in6; else p :inno; end

LIM2 = 1
module Ns
  LIM2 = 3
  class K; def self.===(o, a = LIM2) = o == a; end
end
case 3; when Ns::K then p :k3; else p :no; end
case 1; when Ns::K then p :k1; else p :no; end
p [1, 3].grep(Ns::K)

