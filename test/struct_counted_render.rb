# spinel: gc-stress
Link = Struct.new(:next)
Envelope = Struct.new(:link)
node = Link.new(nil)
node.next = node
p Envelope.new(node)
p Envelope.new(nil)
