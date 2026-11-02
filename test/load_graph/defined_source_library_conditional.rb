# A guard whose library is required under a run-time condition, or by a file
# required ahead of it, answers by what has run at the guard: here the
# condition is false, so the library is not loaded and the guard finds none.
require_relative "defined_source_library_conditional_lib"
require "erb" if ARGV.size > 5

if defined?(ERB::Escape)
  def which = "then"
else
  def which = "else"
end
p which
p(defined?(ERB::Escape) ? "then" : "else")
p lib_which
