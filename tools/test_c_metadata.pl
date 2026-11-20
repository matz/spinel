use strict;
use warnings;

# Read generated C from stdin. The first output line holds fixed fields;
# each following line preserves the link inputs in their original order.
binmode STDIN;
my ($pch, $preamble, $lines) = ('plain', 1, 0);
my ($threads, $cext, $nogc) = (0, 0, 0);
my ($libs, $objects) = ('', '');
while (my $line = <STDIN>) {
    my $newline = chomp $line;
    $lines += $newline;
    if ($preamble) {
        if ($line =~ /^#include "spinel_rt\.h"/) {
            $preamble = 0;
        } elsif ($line =~ /^#define/) {
            if ($pch eq 'plain' && $line eq '#define SP_TU_NO_POLY_RENDER 1') {
                $pch = 'nopoly';
            } elsif ($pch eq 'plain' && $line eq '#define SP_TU_NO_POLY_VALUES 1') {
                $pch = 'novalues';
            } else {
                $pch = 'textual';
            }
        }
    }
    $threads = 1 if index($line, 'SPINEL_USES_THREADS') >= 0;
    $cext = 1 if index($line, 'SPINEL_USES_CEXT') >= 0;
    $nogc = 1 if index($line, 'SPINEL_GC_NONE') >= 0;
    my $separator = $newline ? ' ' : '';
    $libs .= $1 . $separator if $line =~ m{^/\* SPINEL_LINK: (.*) \*/\z};
    $objects .= $1 . $separator if $line =~ m{^/\* SPINEL_LINK_OBJ: (.*) \*/\z};
}
my $large = $lines >= 2000 ? 1 : 0;
print "$pch $large $threads $cext $nogc\n$libs\n$objects\n";
