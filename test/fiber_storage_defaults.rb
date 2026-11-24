# spinel: gc-stress
# spinel: gc-minor
# spinel: share
# Defaults belong to the copied storage, including the Hash passed to its proc.
$stderr.reopen(File::NULL)
h = Hash.new(:fallback)
h[:a] = 1
f = Fiber.new(storage: h) do
  p [Fiber[:a], Fiber[:missing], Fiber.current.storage.default]
  Fiber[:a] = nil
  p [Fiber[:a], Fiber.current.storage.key?(:a)]
  copy = Fiber.current.storage
  copy[:only_copy] = 2
  p Fiber[:only_copy]
  p Fiber.new { [Fiber[:missing], Fiber.current.storage.default] }.resume
end
h[:a] = 9
h.default = :changed
f.resume
p h[:a]

h = Hash.new { |hash, key| hash[key] = key.to_s * 2 }
h[:a] = nil
f = Fiber.new(storage: h) do
  GC.start
  p Fiber[:a]
  p Fiber[:b]
  p Fiber.current.storage.default_proc.nil?
  p Fiber.current.storage
  Fiber[:b] = nil
  p Fiber.current.storage.key?(:b)
  p Fiber[:b]
  p Fiber.new { Fiber[:child] }.resume
  p Fiber.current.storage.key?(:child)
  copy = Fiber.current.storage
  p copy[:copy]
  p Fiber.current.storage.key?(:copy)
end
f.resume
p h

Fiber.current.storage = Hash.new('held' * 3)
GC.start
p Fiber[:missing]
p Fiber.current.storage.default
Fiber[:missing] = 7
p Fiber[:missing]
Fiber[:missing] = nil
p Fiber[:missing]
Fiber.current.storage = Hash.new { |hash, key| hash[key] = [key] }
GC.start
p Fiber[:assigned]
p Fiber.new { Fiber[:inherited] }.resume
p Fiber.current.storage.keys
Fiber.current.storage = nil
p Fiber[:missing]

# Retain a captured value only through suspended fibers and inherited stores.
def defaulted_fiber(i)
  value = 'captured' * i
  Fiber.new(storage: Hash.new { |hash, key| hash[key] = value }) do
    GC.start
    p Fiber[:value].size
    p Fiber.new { GC.start; Fiber[:other].size }.resume
  end
end
fibers = (1..3).map { |i| defaulted_fiber(i) }
GC.start
fibers.each(&:resume)

# False is a default too, and ordinary replacement discards the old default.
Fiber.current.storage = Hash.new(false)
p Fiber[:missing]
Fiber.current.storage = {}
p Fiber[:missing]
