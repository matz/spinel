# spinel: gc-stress
# A key's own <=> that raises, again and again, through the typed sort_by and
# the boxed one (a forwarded proc on a value only known at run time): the
# sort's merge buffer belongs to the GC, so unwinding leaves nothing behind.
class Bad
  def <=>(o) = raise("bad")
end

fb = proc { |x| Bad.new }
bad = ARGV.size > 5 ? 1 : [1, 2, 3, 4]
raised = 0
300.times do
  begin
    bad.sort_by(&fb)
  rescue RuntimeError
    raised += 1
  end
  begin
    [1, 2, 3, 4].sort_by { |x| Bad.new }
  rescue RuntimeError
    raised += 1
  end
end
p raised
