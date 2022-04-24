
<script>
    import {onDestroy, onMount} from "svelte";
    import RefreshManager from "./Utils";

    let scripts = {};
    async function activate(script) {
        await fetch("/scripts/" + encodeURIComponent(script) + "/activate", {method: "POST"})
        scripts[script].active = !scripts[script].active;
        await updateStatus();
    }

    async function deactivate(script) {
        await fetch("/scripts/" + encodeURIComponent(script) + "/deactivate", {method: "POST"})
        scripts[script].active = !scripts[script].active;
        await updateStatus();
    }
    async function deleteScript(script) {
        if(!confirm("Are you sure you want to delete " + script + "?")) {
            return;
        }
        let resp = await fetch("/scripts/" + script, {method: "DELETE"});
        if(resp.status !== 200)
            return;
        delete scripts[script]
        scripts = scripts
    }
    async function newScript() {
        let script = prompt("Please enter the name of the new script");
        if(script === null)
            return;
        let resp = await fetch("/scripts/" + script, {method: "PUT"});
        if(resp.status !== 200)
            return;
        scripts[script] = {name: script, active: true}
        // sort
        scripts = Object.keys(scripts).sort().reduce(
            (obj, key) => {
                obj[key] = scripts[key];
                return obj;
            },
            {}
        );
        console.log(scripts)
    }
    let fetchPromise = undefined;
    async function updateStatus() {
        fetchPromise = (async function() {return await (await fetch("/scripts")).json()})();
        scripts = await fetchPromise;

    }

    export let refreshRate;
    let manager = new RefreshManager(updateStatus, refreshRate);
    $: manager.refresh(refreshRate);
</script>

<main>
    <div id="toolbar">
        <span class="toolbar-item" id="newScript" on:click={newScript}>New</span>
    </div>
    <div id="scripts">
    {#each Object.keys(scripts).sort() as script (scripts[script].name + scripts[script].state)}
        <div class="script">
            <div class="scriptName">
                {scripts[script].name}
            </div>
            {#if scripts[script].state === "running"}
                <span on:click={deactivate(script)} class="info-tile info-tile-clickable">
                    Deactivate
                </span>
            {:else if scripts[script].state === "deactivated"}
                <span on:click={activate(script)}  class="info-tile info-tile-clickable">
                    Activate
                </span>
            {:else}
                <span  class="info-tile">
                    {scripts[script].state}
                </span>
            {/if}
            <span onclick="window.location = 'edit.html?script={script}'" class="info-tile info-tile-clickable">
                Edit
            </span>
            <span on:click={deleteScript(script)} class="info-tile info-tile-clickable">
                Delete
            </span>
        </div>
    {/each}
    </div>
</main>

<style>
.script {
    background-color: white;
    border-radius: 10px;
    padding: 10px;
    box-shadow: #9f9f9f 0 0 5px;
    margin-bottom: 20px;
    display: grid;
    grid-template-columns: auto min-content min-content min-content;
    align-items: center;
}
main {
    font-size: var(--medium-font-size);
}
#toolbar {
    margin-bottom: 20px;
    display: flex;
    align-items: center;
}
.toolbar-item {
    border: 2px solid #8be4ff;
    background-color: white;
    box-shadow: #9f9f9f 0 0 5px;
    border-radius: 10px;
    padding: 10px;
    cursor: pointer;
}

.info-tile {
    border: 2px solid rgb(200, 200, 200);
    background-color: rgb(250, 250, 250);
    padding: 5px;
    margin: 5px;
}
.info-tile-clickable {
    cursor: pointer;
    border: 2px solid rgb(150, 150, 150);
    background-color: rgb(240, 240, 240);
    user-select: none;
}
.script > * {
    margin: 0 0 0 10px;
}
</style>