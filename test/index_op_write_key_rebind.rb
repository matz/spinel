# spinel: gc-stress
# The key builds an Array after replacing the variable the receiver came
# from. The write still lands in the original receiver.

a = [1, 2, 3]
old = a
a[(a = [7, 8, 9]; [1, 2].size)] += 5
p old, a

@a = [1, 2, 3]
@a[(@a = [7, 8, 9]; [1, 2].size)] += 5
p @a

h = {2 => 10}
old_hash = h
p(h[(h = {2 => 20}; [1, 2].size)] += 5)
p old_hash, h

a = [1, 2, 3]
swap = -> { a = [7, 8, 9]; 1 }
a[[swap.call][0]] += 7
p a

def receiver
  puts "receiver"
  [20, 30]
end

def key
  puts "key"
  [1, 2]
end

p(receiver[[key.size - 2][0]] += 5)
