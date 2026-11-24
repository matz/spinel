# spinel: gc-stress
# spinel: gc-minor
# spinel: share
$stderr.reopen(File::NULL)

def empty_string_storage
  h = {'seed' => 1, 'other' => 'value'}
  captured = 'kept' * 2
  h.default_proc = proc do |hash, key|
    GC.start
    hash[key] = [key.class.name, key.to_s, captured]
  end
  h.clear
  h
end

source = empty_string_storage
f = Fiber.new(storage: source) do
  p Fiber[:missing]
  p Fiber.current.storage.key?(:missing)
  Fiber[:missing] = nil
  p Fiber.current.storage.key?(:missing)
  p Fiber[:missing]
  p Fiber.new { Fiber[:child] }.resume
  p Fiber.current.storage.key?(:child)
  snapshot = Fiber.current.storage
  p snapshot[:copy]
  p snapshot['string']
  p snapshot.default_proc.call(snapshot, :explicit)
  p Fiber.current.storage.key?(:copy)
end
GC.start
f.resume
p source

Fiber.current.storage = empty_string_storage
GC.start
p Fiber[:assigned]
p Fiber.current.storage[:snapshot]
p Fiber.new { Fiber[:inherited] }.resume
p Fiber.current.storage.keys
Fiber.current.storage = nil

def copied_string_default
  h = {'seed' => 1, 'other' => 'value'}
  h.default_proc = proc { |hash, key| key.to_s }
  h.clear
  f = Fiber.new(storage: h) { GC.start; Fiber[:retained] }
  h.default_proc = proc { |hash, key| "changed" }
  f
end
p copied_string_default.resume
