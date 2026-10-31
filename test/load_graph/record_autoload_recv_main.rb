# Code the load record has not seen (--dump-load-record,
# record_autoload_recv_main.rb.load-record): an autoload with a receiver,
# which the parse leaves in the tree, loads a file when its constant is
# first read, so a version of Box#get may run outside its interval. Only a
# require or a load on no receiver, Object or Kernel counted, and an
# autoload is one on any receiver.
# Graph mode now registers this closed target and sees every file.
class Box
  def get = 1
  def get = 2
end
self.autoload(:Later, File.join(__dir__, "autoload_lib")) if ARGV.size > 9
p Box.new.get
