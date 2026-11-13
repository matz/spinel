# A listing or a dynamic call named by a Symbol (send's, to_proc's, a
# block from &:respond_to?) sees every method.
class T
  def secret = "s"
  def last_one = :kept
end
p T.send(:instance_methods, false).sort
p :instance_methods.to_proc.call(T, false).sort
p [T.new, :secret].inject(&:respond_to?)
