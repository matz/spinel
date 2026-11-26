# A call whose count fails raises once its arguments have run, and those
# arguments are bound to their temps while it is written. The bindings
# stayed after the call, and the table holding them is never emptied for
# the whole program, so once it filled, a later method's each found no
# route, a break-wrapped each refused its receiver, and `&.` wrote its
# receiver in place of its call.
$n = 0
def one(a) = a
def side = ($n += 1)

class Early
  def bad
    one(side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side, side)
  rescue ArgumentError => e
    e.message
  end
end

class Box
  def initialize
    @items = { "a" => 1, "b" => 2 }
  end

  def keys_each
    @items.keys.each { |k| puts k }
  end

  def select_each
    @items.values.select { |v| v > 1 }.each { |v| puts v }
  end

  def break_each
    got = nil
    make.each do |v|
      got = v
      break if v > 1
    end
    got
  end

  def make
    $n += 100
    [1, 2, 3]
  end

  def count_of(row)
    row["count"]&.to_i
  end
end

puts Early.new.bad
b = Box.new
b.keys_each
b.select_each
p b.break_each
p b.count_of({ "count" => "5" })
p b.count_of({ "count" => 2.5 })
p b.count_of({})
p $n
