# The hook that runs a reopened StandardError#initialize on an exception the runtime built clears the
# message the runtime gave it only in an arm that runs an initialize. Here the message parameter is
# typed Integer (StandardError.new(5) is its only literal caller), which no arm takes, so the Errno built
# through a class value must keep its errno text: it used to lose it and answer the class name.
# (CRuby runs the initialize on the String it is given and answers "xBroken pipe - w"; the text is
# checked by what it contains.)
class StandardError
  def initialize(m = 0) = super("x#{m}")
end
p StandardError.new(5).message
k = Errno::EPIPE
p k.new("w").message.include?("Broken pipe")
