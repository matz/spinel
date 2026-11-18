# A captured method a direct call typed Integer, reached through a Hash
# beside a lambda: every argument the dynamic call hands it is an Integer the
# compiler names, none boxed, so its parameter and return keep the sp_int
# lane (method_capture_boxed_arg.rb is the program whose boxed argument
# widens one).
class Counter
  def initialize = @total = 0
  def add(amount)
    @total += amount
    @total
  end
end
def run(table, key, n) = table[key].call(n)
c = Counter.new
c.add(1)
table = {add: c.method(:add), twice: ->(v) { v * 2 }}
p run(table, :add, 2)
p run(table, :twice, 3)
p c.add(4) + 1
