
<script>
    import {EditorView} from "@codemirror/view"
    import {basicSetup} from "@codemirror/basic-setup"
    import {EditorState} from "@codemirror/state"
    import {javascript} from "@codemirror/lang-javascript"
    import {onMount} from "svelte";

    let myView;
    let file = new URLSearchParams(window.location.search).get("script");
    onMount(async () => {
        myView = new EditorView({
            state: EditorState.create({extensions: [basicSetup, javascript()]}),
        })
        let resp = await fetch("/scripts/" + file);
        if(resp.status !== 200) {
            console.error(await resp.text());
            return;
        }
        let contentJson = await resp.json();
        let code = contentJson.code;
        myView.dispatch({
            changes: {from: 0, to: myView.state.doc.length, insert: code}
        })

        document.getElementById("editor").appendChild(myView.dom);
    })
    async function saveScript() {
        try {
            await fetch("/scripts/" + file, {method: "PUT", body: myView.state.doc.toString()});
        } catch(e) {
            alert("Failed to upload: " + e);
        }
    }
</script>


<main>
    <button on:click={saveScript}>Save</button>
    <div id="editor"></div>
</main>
<style>
    #editor {
        font-size: var(--small-font-size);
        margin-bottom: 100px;
        word-break: break-word;
    }
</style>