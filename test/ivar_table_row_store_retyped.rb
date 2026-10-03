# A row stored into an ivar table through methods written below the ones
# they call: `@banks[0] = a1`, where a1 first answers the int array an
# append on a receiver not typed yet makes, and a round later the boxed
# Array it really is. The table pass pinned @banks to int arrays on the
# first answer and did not look at a pinned table again, so the boxed row
# was read back as a bare sp_IntArray * ([0, 8, 0, 0, 0, 0, 0, 0]). A pinned
# table is now vetted again on every round. With the table's other readers
# beside the store, and a table whose rows stay int arrays.
class Deep
  def initialize
    @banks = [[1]]
    @patterns = [[]]
  end
  def bank
    @banks[0] = a1
    @banks[0]
  end
  def a4
    h = @patterns[0]
    h << 7
    h << 8
    h
  end
  def a3; a4; end
  def a2; a3; end
  def a1; a2; end
end
p Deep.new.bank

class Uses
  def initialize
    @banks = [[1], [2, 3]]
    @patterns = [[]]
  end
  def bank
    @banks[0] = a1
    row = @banks[0]
    row[0]
  end
  def sum
    t = 0
    @banks.each { |r| t += r[0] }
    t
  end
  def cell(i) = @banks[i][0]
  def take(x) = x + 1
  def pass = take(@banks[1][1])
  def a4
    h = @patterns[0]
    h << 7
    h
  end
  def a3; a4; end
  def a2; a3; end
  def a1; a2; end
end
u = Uses.new
p u.bank, u.sum, u.cell(1), u.pass

class Typed
  def initialize
    @rows = [[1]]
  end
  def fill
    @rows[0] = r1
    @rows << []
    @rows[1] << 4
    @rows[0][1] + @rows[1][0]
  end
  def r3 = [2, 3]
  def r2 = r3
  def r1 = r2
end
p Typed.new.fill
