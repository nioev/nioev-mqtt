<script>
    import Chart from "chart.js/auto";
    import {onMount} from "svelte";

    function toScatterData(src, interval) {
        let values = [];
        let keys = Object.keys(src)
        let min = Math.min(...keys);
        let max = Math.max(...keys);
        for(let i = min; i <= max; i += interval) {
            let datapoint = src[i];
            if(datapoint === undefined) {
                console.log("Missing datapoint for element " + (i - min).toString() + "(" + i + ")");
                console.log(src, src[i]);
                values.push({x: i, y: 0});
            } else {
                values.push({x: i, y: datapoint.msg_count})
            }
        }
        return {values: values, min: min, max: max};
    }

    function drawHistogram(canvasId, color, scatterData) {
        let min = scatterData.min;
        let max = scatterData.max;
        let values = scatterData.values;

        const data = {
            datasets: [{
                label: 'test',
                backgroundColor: color,
                borderColor: color,
                data: values,
            }]
        };
        Chart.defaults.font.size = 16;
        const config = {
            type: 'scatter',
            data: data,
            options: {
                showLine: true,
                elements: {
                    point:{
                        radius: 0
                    }
                },
                plugins: {
                    legend: {
                        display: false
                    },
                    tooltip: {
                        callbacks: {
                            label: function(context) {
                                return new Date(context.parsed.x * 1000).toLocaleTimeString('de-DE') + ": " + context.parsed.y;
                            }
                        }
                    },
                },
                scales: {
                    x: {
                        ticks: {
                            callback: function (value) {
                                return new Date(value * 1000).toLocaleTimeString('de-DE');
                            },
                        },
                        min: min,
                        max: max
                    },
                    y: {
                        min: 0
                    }
                },
            }
        };
        let c = new Chart(
            document.getElementById(canvasId),
            config
        );

        return function(scatterData) {
            c.data.datasets.forEach((dataset) => {
                dataset.data = scatterData.values;
            })
            c.options.scales.x.min = scatterData.min;
            c.options.scales.x.max = scatterData.max;
            c.update();
        }
    }

    let statsPromise = fetch("/statistics");
    onMount(async () => {
        let stats = await (await statsPromise).json()

        let graphSeconds = drawHistogram('messagesPerSecond', 'rgb(255, 99, 132)', toScatterData(stats.msg_per_second, 1));
        let graphMinutes = drawHistogram('messagesPerMinute', 'rgb(211,99,255)', toScatterData(stats.msg_per_minute, 60));
        setInterval(async () => {
            let stats = await (await fetch("/statistics")).json();
            graphSeconds(toScatterData(stats.msg_per_second, 1));
            graphMinutes(toScatterData(stats.msg_per_minute, 60));
        }, 15 * 1000);
    });
</script>

<main>
    <p class="control">Version: xyz</p>
    <p class="control">Uptime: xyz</p>
    <div class="control" >
        <center>Messages per Second</center>
        <canvas id="messagesPerSecond"></canvas>
    </div>
    <div class="control" >
        <center>Messages per Minute</center>
        <canvas id="messagesPerMinute"></canvas>
    </div>
    <p class="control">Test</p>
    <p class="control">Test</p>
    <p class="control">Test</p>
    <p class="control">Test</p>
</main>

<style>
    main {
        display: grid;
        grid-template-columns: calc(50% - 10px) calc(50% - 10px);
        grid-auto-rows: auto;
        gap: 20px;
    }
    .control {
        background-color: white;
        border-radius: 10px;
        box-shadow: #9f9f9f 0 0 5px;
        padding: 10px;
        margin: 0;
        font-size: 20px;
    }
</style>