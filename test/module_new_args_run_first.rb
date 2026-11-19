# `new` on a module raises NoMethodError, after its arguments ran in source
# order, whatever their form: a positional beside an Array literal (whose
# build hoists), a keyword, a splat, a block argument.
module M; end

def mark(x)
  puts "mark #{x}"
  x
end

begin
  M.new(mark(1), [mark(2)])
rescue NoMethodError => e
  puts e.message
end
begin
  M.new(mark(3), k: mark(4))
rescue NoMethodError => e
  puts e.message
end
a = [mark(5)]
begin
  M.new(*a, mark(6))
rescue NoMethodError => e
  puts e.message
end
begin
  M.new(mark(7), &proc { mark(8) })
rescue NoMethodError => e
  puts e.message
end
puts mark(9)
