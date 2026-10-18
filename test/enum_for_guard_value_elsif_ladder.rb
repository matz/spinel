# A method whose block form answers a value through an `elsif` ladder of 14
# arms, the value in the last arm, and whose blockless call answers the
# Enumerator of a `return to_enum(:m) unless block_given?` guard. The search
# for a value in the method's tail gave up at a depth of 12, and an arm of the
# ladder cost a level, so the method was taken for an each-like one whose value
# nobody keeps: its return was pinned to the Enumerator, and the Array the
# last arm answers was assigned to an sp_Enumerator in the C. The tail is read
# to the end of the ladder.
class Coll
  def initialize; @a = [1, 2, 3]; @k = 99; end
  def pick(k)
    @k = k
    run { |x| x }
  end
  def run
    return to_enum(:run) unless block_given?
    if @k == 0 then yield(0)
    elsif @k == 1 then yield(1)
    elsif @k == 2 then yield(2)
    elsif @k == 3 then yield(3)
    elsif @k == 4 then yield(4)
    elsif @k == 5 then yield(5)
    elsif @k == 6 then yield(6)
    elsif @k == 7 then yield(7)
    elsif @k == 8 then yield(8)
    elsif @k == 9 then yield(9)
    elsif @k == 10 then yield(10)
    elsif @k == 11 then yield(11)
    elsif @k == 12 then yield(12)
    elsif @k == 13 then yield(13)
    else @a
    end
  end
end

c = Coll.new
p c.pick(99)
p c.pick(3)
p c.run.class
p c.run.to_a
