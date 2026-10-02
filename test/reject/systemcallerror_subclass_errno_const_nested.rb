# The Errno constant is refused wherever the class body defines it: under a
# condition, and in a reopening of the class.
class MyErr < SystemCallError
  if ARGV.empty?
    Errno = 13
  end
end
class MyErr2 < Errno::ENOENT; end
class MyErr2
  Errno = 2
end
p MyErr.new("m").errno, MyErr2.new("m").errno
