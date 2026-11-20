# A reopening of LoadError, NoMemoryError, SystemStackError or SystemCallError that defines initialize
# and that nothing else in the program names. The runtime builds these classes whether or not the program
# writes a `new`, and the hook that runs a reopened initialize on an error the runtime raises
# (sp_user_exc_reinit) names the initialize, so it has to be emitted. These four are not builtin class
# names (only exceptions), so they were held back as classes nothing builds and the program failed in
# the C compiler: call to undeclared function 'sp_SystemCallError_initialize'.
class LoadError
  def initialize(m = nil) = super("LE[#{m}]")
end
class NoMemoryError
  def initialize(m = nil) = super("NM[#{m}]")
end
class SystemStackError
  def initialize(m = nil) = super("SS[#{m}]")
end
class SystemCallError
  def initialize(m = nil, f = nil) = super("SCE[#{m}|#{f}]")
end
p :built
