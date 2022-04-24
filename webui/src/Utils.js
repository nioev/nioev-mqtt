import {onDestroy, onMount} from "svelte";

export default class RefreshManager {
    oldRefreshRate;
    refreshCallback;
    constructor(refreshCallback, refreshRate) {
        this.refreshCallback = refreshCallback;
        this.refreshIntervalTimeoutId = 0;
        this.initDone = false;
        onMount(async () => {
            await refreshCallback();
            this.initDone = true;
            this.oldRefreshRate = refreshRate;
            this.refreshIntervalTimeoutId = setInterval(refreshCallback, refreshRate);
        });
        window.addEventListener("pageshow", function(event) {
            if (event.persisted) {
                refreshCallback();
            }
        });

        onDestroy(() => {
            clearInterval(this.refreshIntervalTimeoutId);
        })
    }
    refresh(refreshRate) {
        console.log("Refresh rate: ", refreshRate);
        if (this.oldRefreshRate !== refreshRate && this.initDone) {
            this.oldRefreshRate = refreshRate;
            (async () => {
                clearInterval(this.refreshIntervalTimeoutId);
                await this.refreshCallback();
                this.refreshIntervalTimeoutId = setInterval(this.refreshCallback, refreshRate);
            })();
        }
    }
}