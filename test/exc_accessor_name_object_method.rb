# A method the program adds to Object under the name of a class-gated
# exception accessor (tag, key, status, name, ...) answers for every
# exception class but the ones owning the accessor, which keep it.
class Object
  def tag = :t
  def key = "k"
  def status = 42
  def name = "nm"
end
p RuntimeError.new("x").tag
p ("a".."c").tag
p ArgumentError.new.key, StandardError.new.status
e = UncaughtThrowError.new(:sym, 5, "m") rescue nil
begin
  throw :zz, 3
rescue UncaughtThrowError => u
  p u.tag
end
begin
  {a: 1}.fetch(:b)
rescue KeyError => k
  p k.key
end
begin
  exit 3
rescue SystemExit => s
  p s.status
end
p RuntimeError.new.name
begin
  zzq
rescue NameError => n
  p n.name
end
x = [RuntimeError.new, 1][0]
p x.tag

# Private Object methods reject an explicit exception receiver; a protected
# Object method remains callable from the top-level Object receiver.
class Object
  private
  def value = :private_value
  protected
  def receiver = :protected_receiver
end
begin
  p RuntimeError.new.value
rescue NoMethodError
  puts :private_rejected
end
begin
  p RuntimeError.new.receiver
rescue NoMethodError
  puts :protected_rejected
end
begin
  throw :private_name, 7
rescue UncaughtThrowError => u
  p u.value
end
p StopIteration.new.result
