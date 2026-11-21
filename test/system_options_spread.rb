# system's options spread from a Hash value (`**opts`), after an environment
# Hash too, reach the spawn as options written as keyword arguments do
# (test/system_options.rb): the call's tree says it carries keywords, where
# its text shows none.
opts = {exception: true}
p system({"A" => "1"}, "echo [$A]", **opts)
quiet = {out: File::NULL}
p system("sh", "-c", "echo hidden; exit 3", **quiet)
begin
  system("sh", "-c", "exit 4", **opts)
rescue RuntimeError => e
  puts e.message
end
