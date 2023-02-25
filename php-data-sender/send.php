<?php

$address = '192.168.1.85';
$port = 1337;
$beat_period = 3;

$elements = [];

$usage = 10;

// Modify the string
if (filter_input(INPUT_POST, 'color', FILTER_SANITIZE_NUMBER_INT)!=="") {
	$elements[] = filter_input(INPUT_POST, 'color', FILTER_SANITIZE_NUMBER_INT) . ".0";
} else {
    die("Missing POST parameter 'color'");
}
if (filter_input(INPUT_POST, 'usage', FILTER_SANITIZE_NUMBER_INT)!=="") {
    // Normalize to 0..10 to translate to segments 1..10
    $usage = filter_input(INPUT_POST, 'usage', FILTER_SANITIZE_NUMBER_INT) / 10;	
} else {
    die("Missing POST parameter 'usage'");
}
if (filter_input(INPUT_POST, 'segment', FILTER_SANITIZE_NUMBER_INT)!=="") {
    for ($i = 1; $i <= 10; $i++) {
		$elements[] = ($i<=$usage)?filter_input(INPUT_POST, 'segment', FILTER_SANITIZE_NUMBER_INT) . ".0" : "0.0";
	}		
} else {
    die("Missing POST parameter 'segment'");
}

$fp = stream_socket_client("udp://$address:$port", $errno, $errstr);
if (!$fp) {
 die("ERROR: $errno - $errstr");
}

$message = implode(",", $elements);
fwrite($fp, $message);
echo "sent $message";