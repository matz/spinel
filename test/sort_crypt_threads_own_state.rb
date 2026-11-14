# Array#sort of a boxed Array and String#crypt keep their state per call:
# threads running them at once each get their own answer.
inputs = [[3, 1, 2], [1.5, nil, 2]]
bad = 0
lock = Mutex.new
threads = Array.new(8) do |i|
  Thread.new do
    k = i % 2
    2_000.times do
      v = begin
        inputs[k].sort
      rescue ArgumentError
        :raised
      end
      lock.synchronize { bad += 1 } if k == 0 && v != [1, 2, 3]
    end
  end
end
threads.each(&:join)
p bad
words = Array.new(8) { |i| "pw#{i}" }
want = words.map { |w| w.crypt("ab") }
bad2 = 0
lock2 = Mutex.new
threads2 = Array.new(8) do |i|
  Thread.new do
    n = 0
    300.times { n += 1 if words[i].crypt("ab") != want[i] }
    lock2.synchronize { bad2 += n }
  end
end
threads2.each(&:join)
p bad2
