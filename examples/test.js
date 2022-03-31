function onPublish(arg) {
    if(arg.retained) {
        console.log("Message was just retained...");
        return;
    }
    console.log("Received publish: ", arg);
}

let pixels = []
for(let i = 0; i < 30; ++i) {
    pixels.push(255);
    pixels.push(255);
    pixels.push(255);
    pixels.push(255);
}
nv.publish("sbcs/ledstrip-esp/leds/leds", new Uint8Array(pixels), true, 2)
nv.publish("sbcs/ledstrip-esp/leds/brightness", "0", true, 2);

console.log("test.js loaded");
nv.subscribe("test", "xyz", "topic3")
