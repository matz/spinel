# SystemCallError#initialize reads the errno through the class's own Errno
# constant; the runtime knows only the Errno classes' numbers by name, so a
# subclass defining one is refused rather than given another number.
class MyErr < SystemCallError
  Errno = 2
end
p MyErr.new("m").errno
