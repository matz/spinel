# A fold's accumulator that a nested block captures, where the receiver may
# be an Array or anything else: the block parameter is boxed, so its cell is
# declared boxed, and the inline Array fold bound it as the seed's own Array
# kind -- the fresh cell each turn and the reads through it disagreed with
# that declaration and the C build failed (fugit's Cron#compact_month_days).
class Bag
  def each
  end
end

class Cron
  def compact(flag)
    return :skipped if flag
    @months.inject([[], []]) { |a, m|
      @monthdays.each { |d| a[1] << d }
      a }
  end
end

p Cron.new.compact(true)
begin
  Cron.new.compact(false)
rescue NoMethodError => e
  puts e.message
end
