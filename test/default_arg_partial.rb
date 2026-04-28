# Issue #49 (mixed case): caller provides some positional args but
# omits trailing defaulted ones. The omitted slots need their defaults
# substituted, not zero-filled.

class M
  def f(a, b = 1, c = 2)
    puts a + b + c
  end
end

m = M.new
m.f(10)              # 10 + 1 + 2  = 13
m.f(10, 20)          # 10 + 20 + 2 = 32
m.f(10, 20, 30)      # 10 + 20 + 30 = 60
