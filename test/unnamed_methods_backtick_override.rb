# A method defined as `` ` `` is named by every backtick literal: an override a
# backtick reaches is not left out as a method nothing names.
module Declaration
  def `(command) = "ran #{command}"

  def description
    `unused_command`
  end
end

class StaticAction
  extend Declaration
end

class Shell
  def `(command) = command.upcase

  def run(x) = `echo #{x}`
end

puts StaticAction.description
puts Shell.new.run("hi")
