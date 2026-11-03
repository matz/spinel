# `in [Path::X, Integer]` through a flagged parent
class Tgt; X = 10; end
module M
  def self.a1(v)
    case v
    in [M::LP::X, Integer] then p :arr
    else p :noarr
    end
  rescue NameError => e
    puts "a1: #{e.message}"
  end
end
M.a1([1, 2])
M::LP = Tgt
M.a1(5); M.a1([1, 2]); M.a1([10, 2])
