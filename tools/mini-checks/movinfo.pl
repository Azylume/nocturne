#!/usr/bin/perl
# Prints "SECONDS VIDEO_TRACKS AUDIO_TRACKS [CODEC CHANNELS RATE]" of a QuickTime/MP4 file from its moov atom (a finalized
# movie), or "unfinished 0 0". CODEC CHANNELS RATE describe the first audio track (aac for mp4a).
# For the Mini, which has no ffprobe: a movie whose writer finished has a moov with mvhd and a video handler.
use strict;
use warnings;

open(my $f, '<:raw', $ARGV[0]) or die "cannot open $ARGV[0]\n";
my $size = -s $f;

sub atoms {
    my ($from, $to, $want) = @_;
    my @found;
    my $at = $from;
    while ($at + 8 <= $to) {
        seek($f, $at, 0);
        read($f, my $head, 16) >= 8 or last;
        my ($len, $type) = unpack('N a4', $head);
        my $hdr = 8;
        if ($len == 1) { $len = unpack('x8 Q>', $head); $hdr = 16; }
        elsif ($len == 0) { $len = $to - $at; }
        last if $len < $hdr;
        push @found, [$at + $hdr, $at + $len] if $type eq $want;
        $at += $len;
    }
    return @found;
}

my ($moov) = atoms(0, $size, 'moov');
unless ($moov) { print "unfinished 0 0\n"; exit 0; }
my ($mvhd) = atoms(@$moov, 'mvhd');
seek($f, $mvhd->[0], 0);
read($f, my $m, 32);
my $version = unpack('C', $m);
my ($scale, $duration) = $version == 1 ? unpack('x20 N Q>', $m) : unpack('x12 N N', $m);
my ($video, $audio, $format) = (0, 0, '');
for my $trak (atoms(@$moov, 'trak')) {
    for my $mdia (atoms(@$trak, 'mdia')) {
        for my $hdlr (atoms(@$mdia, 'hdlr')) {
            seek($f, $hdlr->[0] + 8, 0);
            read($f, my $kind, 4);
            $video++ if $kind eq 'vide';
            next unless $kind eq 'soun';
            $audio++;
            next if $format;
            my ($minf) = atoms(@$mdia, 'minf');
            my ($stbl) = $minf && atoms(@$minf, 'stbl');
            my ($stsd) = $stbl && atoms(@$stbl, 'stsd');
            next unless $stsd;
            seek($f, $stsd->[0] + 8, 0);
            read($f, my $entry, 36);
            my ($type, $channels, $rate) = unpack('x4 a4 x16 n x6 n', $entry);
            $format = sprintf '%s %d %d', $type eq 'mp4a' ? 'aac' : $type, $channels, $rate;
        }
    }
}
printf "%.2f %d %d%s\n", $scale ? $duration / $scale : 0, $video, $audio, $format ? " $format" : '';
