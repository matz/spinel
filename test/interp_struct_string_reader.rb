# spinel: gc-stress
Label = Struct.new(:to_s)
s = +"frog"
label = Label.new(s)
p "#{label}#{s.chop!}".bytes
s = +"a\0word"
label = Label.new(s)
p label.to_s.equal?(s)
label.display
puts
boxed = [label, 3]
p "#{boxed[0]}#{s.chop!}".bytes
ReadInspect = Struct.new(:inspect)
item = ReadInspect.new(s)
p item.inspect.equal?(s)
p item
