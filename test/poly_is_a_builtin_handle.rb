# A boxed IO, File, Dir, Mutex, Queue or Thread is its own builtin
# class, as #class says, when is_a?, kind_of?, ===, grep or a when arm
# takes the class from a value: Dir and IO are Enumerable, a File is
# an IO, and a Mutex is a Thread::Mutex.
d = Dir.new(File.dirname(__FILE__))
f = File.open(__FILE__)
a = [3, d, $stdin, f, "s", Mutex.new, Queue.new, Thread.current, 1..2, Time.at(0)]
ks = [IO, File, Dir, Enumerable, Comparable, Object, Mutex, Thread::Queue, Thread, Range, Time, Kernel, BasicObject]
a.each { |v| p ks.map { |k| v.is_a?(k) } }
a.each { |v| p ks.map { |k| k === v } }
p a.grep(Enumerable).size
m = Enumerable
a.each { |v| puts "#{v.class} #{v.is_a?(m)} #{m === v} #{v.kind_of?(m)}" }
k = ARGV.size > 3 ? IO : Enumerable
p a.map { |v| case v when k then 1 else 0 end }
k2 = ARGV.size > 3 ? Enumerable : IO
p a.map { |v| v.kind_of?(k2) }
p a.map { |v| v.instance_of?(Dir) }
f.close
d.close
