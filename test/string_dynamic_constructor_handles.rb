# Strings constructed through Class values keep their mutable objects.
def make(which, *args)
  klass = String
  klass = Array if which == 9
  klass.new(*args)
end
src = "\0b".b
a = make(0, src)
b = make(0)
values = [a, b, 75]
new_size = a.method(:size)
p [a.encoding.name, a.bytes, b.encoding.name, a.equal?(src)]
values[0] << "x" * 180
values[1] << "z" * 201
p [values[0].bytesize, a.bytesize, new_size.call, src.bytesize, b.bytesize]

def allocate_in(which)
  klass = String
  klass = Array if which == 9
  klass.allocate
end
a = allocate_in(0)
b = allocate_in(0)
box = [a, b, nil]
allocated_size = a.method(:bytesize)
p [a.equal?(b), a.encoding.name]
box[0] << "x" * 180
p [box[0].bytesize, a.bytesize, allocated_size.call, b.bytesize]
