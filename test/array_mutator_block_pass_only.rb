# push, append, unshift, prepend and concat on a typed Array given only a
# block operand add nothing and answer the Array: a block-pass (a Symbol, a
# Proc, a Method, nil, an object with to_proc) is not a positional value, and
# the mutator never calls the block. The operand is still converted (an
# a bad one raises TypeError), and a frozen Array raises FrozenError.
pr = proc { |v| v }
m = 5.method(:+)

a = [1, 2]
p a.push(&:x), a.append(&:x), a.unshift(&:x), a.prepend(&:x), a.concat(&:x)
p a.push(&pr), a.append(&pr), a.unshift(&pr), a.prepend(&pr), a.concat(&pr)
p a.push(&m), a.unshift(&m)
p a.push(&nil), a.append(&nil)
p a.push { |v| v }, a.append { }
p a.equal?(a.push(&:x)), a.push(&:x).size

s = ["a"]
f = [1.5]
e = []
p s.push(&:x), f.unshift(&:x), e.append(&:x), e.concat(&:x)
b = [[1], [2]]
p b.push(&:x)

x = [1]
p x&.push(&:x)
n = nil
p n&.push(&:x)
y = [3]
y.push(&:x)
y.push(4, &:x)
p y

[1, 1.5, "s", :s, true].each do |bad|
  begin
    a.push(&bad)
  rescue TypeError => err
    puts err.message
  end
end

fz = [1].freeze
%i[push append unshift prepend].each do |name|
  begin
    fz.public_send(name, &:x)
  rescue FrozenError => err
    puts "#{name}: FrozenError"
  end
end
begin
  fz.push(&:x)
rescue FrozenError
  puts "push: FrozenError"
end
begin
  fz.concat(&:x)
rescue FrozenError
  puts "concat: FrozenError"
end
